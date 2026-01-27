#include "accounts_context_menu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <memory>
#include <random>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <imgui.h>

#include "accounts_join_ui.h"
#include "components/data.h"
#include "console/console.h"
#include "network/roblox/auth.h"
#include "network/roblox/common.h"
#include "network/roblox/games.h"
#include "network/roblox/session.h"
#include "network/roblox/social.h"
#include "system/multi_instance.h"
#include "system/roblox_launcher.h"
#include "ui/ui.h"
#include "ui/webview/webview.h"
#include "ui/widgets/bottom_right_status.h"
#include "ui/widgets/context_menus.h"
#include "ui/widgets/modal_popup.h"
#include "utils/account_utils.h"
#include "utils/thread_task.h"

namespace {
    constexpr int MULTI_EDIT_SENTINEL = -2;
    constexpr int WEBVIEW_CONFIRM_THRESHOLD = 3;

    struct EditNoteState {
            char buffer[1024] {};
            int accountId {-1};
    };

    struct UrlPopupState {
            bool open {false};
            int accountId {-1};
            char buffer[256] {};
    };

    struct MultiUrlPopupState {
            bool open {false};
            int anchorAccountId {-1};
            char buffer[256] {};
    };

    EditNoteState g_editNote;
    UrlPopupState g_customUrl;
    MultiUrlPopupState g_multiUrl;
    std::unordered_set<int> g_presenceFetchInFlight;

    void copyInto(std::span<char> dst, std::string_view src) {
        if (dst.empty()) {
            return;
        }

        const std::size_t max_copy_len = dst.size() - 1;
        const std::size_t len = std::min(src.size(), max_copy_len);

        std::copy(src.begin(), src.begin() + len, dst.begin());

        dst[len] = '\0';
    }

    template<typename Getter>
    std::string joinSelectedField(const std::vector<const AccountData *> &accounts, Getter getter) {
        std::string result;
        for (const auto *acc: accounts) {
            if (!result.empty()) {
                result += '\n';
            }
            result += getter(*acc);
        }
        return result;
    }

    std::string generateBrowserTracker() {
        thread_local std::mt19937_64 rng {std::random_device {}()};
        thread_local std::uniform_int_distribution<int> d1(100000, 130000);
        thread_local std::uniform_int_distribution<int> d2(100000, 900000);
        return std::to_string(d1(rng)) + std::to_string(d2(rng));
    }

    std::string generateLaunchUri(std::string_view ticket, std::string_view placeId, std::string_view jobId) {
        const auto nowMs
            = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                  .count();

        std::string placeLauncherUrl = std::format(
            "https://assetgame.roblox.com/game/PlaceLauncher.ashx?request=RequestGame&placeId={}",
            placeId
        );

        if (!jobId.empty()) {
            placeLauncherUrl += std::format("&gameId={}", jobId);
        }

        return std::format(
            "roblox-player://1/1+launchmode:play+gameinfo:{}+launchtime:{}+browsertrackerid:{}+placelauncherurl:{}+robloxLocale:en_us+gameLocale:en_us",
            ticket,
            nowMs,
            generateBrowserTracker(),
            placeLauncherUrl
        );
    }

    void asyncFetchPresence(int accountId, std::string_view userId, std::string_view cookie) {
        if (g_presenceFetchInFlight.contains(accountId)) {
            return;
        }

        g_presenceFetchInFlight.insert(accountId);
        ThreadTask::fireAndForget([accountId, userIdStr = std::string(userId), cookieCopy = std::string(cookie)]() {
            try {
                const uint64_t uid = std::stoull(userIdStr);
                const auto presenceMap = Roblox::getPresences({uid}, cookieCopy);

                if (const auto it = presenceMap.find(uid); it != presenceMap.end()) {
                    for (auto &acc: g_accounts) {
                        if (acc.id == accountId) {
                            acc.placeId = it->second.placeId;
                            acc.jobId = it->second.jobId;
                            break;
                        }
                    }
                }
            } catch (...) {
                // Silently handle errors
            }
            g_presenceFetchInFlight.erase(accountId);
        });
    }

    void renderCopyInfoMenuSingle(const AccountData &account) {
        if (ImGui::MenuItem("Display Name")) {
            ImGui::SetClipboardText(account.displayName.c_str());
        }
        if (ImGui::MenuItem("Username")) {
            ImGui::SetClipboardText(account.username.c_str());
        }
        if (ImGui::MenuItem("User ID")) {
            ImGui::SetClipboardText(account.userId.c_str());
        }

        ImGui::Separator();

        const bool hasCookie = !account.cookie.empty();

        ImGui::PushStyleColor(ImGuiCol_Text, getStatusColor("Warned"));
        if (ImGui::MenuItem("Cookie", nullptr, false, hasCookie)) {
            ImGui::SetClipboardText(account.cookie.c_str());
        }
        ImGui::PopStyleColor();

        ImGui::PushStyleColor(ImGuiCol_Text, getStatusColor("Warned"));
        if (ImGui::MenuItem("Launch Link", nullptr, false, hasCookie)) {
            ThreadTask::fireAndForget([cookie = account.cookie,
                                       placeId = std::string(join_value_buf),
                                       jobId = std::string(join_jobid_buf)]() {
                const auto ticket = Roblox::fetchAuthTicket(cookie);
                if (ticket.empty()) {
                    return;
                }

                const auto uri = generateLaunchUri(ticket, placeId, jobId);
                ImGui::SetClipboardText(uri.c_str());
            });
        }
        ImGui::PopStyleColor();
    }

    void renderCopyInfoMenuMulti(const std::vector<const AccountData *> &selectedAccounts) {
        if (ImGui::MenuItem("Display Name")) {
            const auto text = joinSelectedField(selectedAccounts, [](const auto &a) {
                return a.displayName;
            });
            ImGui::SetClipboardText(text.c_str());
        }
        if (ImGui::MenuItem("Username")) {
            const auto text = joinSelectedField(selectedAccounts, [](const auto &a) {
                return a.username;
            });
            ImGui::SetClipboardText(text.c_str());
        }
        if (ImGui::MenuItem("User ID")) {
            const auto text = joinSelectedField(selectedAccounts, [](const auto &a) {
                return a.userId;
            });
            ImGui::SetClipboardText(text.c_str());
        }

        ImGui::Separator();

        const bool anyCookie = std::ranges::any_of(selectedAccounts, [](const auto *acc) {
            return !acc->cookie.empty();
        });

        ImGui::PushStyleColor(ImGuiCol_Text, getStatusColor("Warned"));
        if (ImGui::MenuItem("Cookie", nullptr, false, anyCookie)) {
            std::string result;
            for (const auto *acc: selectedAccounts) {
                if (acc->cookie.empty()) {
                    continue;
                }
                if (!result.empty()) {
                    result += '\n';
                }
                result += acc->cookie;
            }
            if (!result.empty()) {
                ImGui::SetClipboardText(result.c_str());
            }
        }
        ImGui::PopStyleColor();

        ImGui::PushStyleColor(ImGuiCol_Text, getStatusColor("Warned"));
        if (ImGui::MenuItem("Launch Link", nullptr, false, anyCookie)) {
            std::vector<std::pair<int, std::string>> accounts;
            accounts.reserve(selectedAccounts.size());
            for (const auto *acc: selectedAccounts) {
                if (!acc->cookie.empty()) {
                    accounts.emplace_back(acc->id, acc->cookie);
                }
            }

            ThreadTask::fireAndForget(
                [accounts, placeId = std::string(join_value_buf), jobId = std::string(join_jobid_buf)]() {
                    std::string result;
                    for (const auto &[id, cookie]: accounts) {
                        const auto ticket = Roblox::fetchAuthTicket(cookie);
                        if (ticket.empty()) {
                            continue;
                        }

                        if (!result.empty()) {
                            result += '\n';
                        }
                        result += generateLaunchUri(ticket, placeId, jobId);
                    }
                    if (!result.empty()) {
                        ImGui::SetClipboardText(result.c_str());
                    }
                }
            );
        }
        ImGui::PopStyleColor();
    }

    void renderNoteMenuSingle(AccountData &account) {
        if (ImGui::MenuItem("Copy Note")) {
            ImGui::SetClipboardText(account.note.c_str());
        }

        if (ImGui::BeginMenu("Edit Note")) {
            if (g_editNote.accountId != account.id) {
                copyInto(g_editNote.buffer, account.note);
                g_editNote.accountId = account.id;
            }

            ImGui::PushItemWidth(ImGui::GetFontSize() * 15.625f);
            ImGui::InputTextMultiline(
                "##EditNoteInput",
                g_editNote.buffer,
                sizeof(g_editNote.buffer),
                ImVec2(0, ImGui::GetTextLineHeight() * 4)
            );
            ImGui::PopItemWidth();

            if (ImGui::Button("Save##Note")) {
                if (g_editNote.accountId == account.id) {
                    account.note = g_editNote.buffer;
                    Data::SaveAccounts();
                }
                g_editNote.accountId = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndMenu();
        }

        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, getStatusColor("Banned"));
        if (ImGui::MenuItem("Clear Note")) {
            account.note.clear();
            Data::SaveAccounts();
        }
        ImGui::PopStyleColor();
    }

    void renderNoteMenuMulti(std::vector<AccountData *> &selectedAccounts) {
        if (ImGui::MenuItem("Copy Note")) {
            std::string result;
            for (const auto *acc: selectedAccounts) {
                if (!result.empty()) {
                    result += '\n';
                }
                result += acc->note;
            }
            ImGui::SetClipboardText(result.c_str());
        }

        if (ImGui::BeginMenu("Edit Note")) {
            if (g_editNote.accountId != MULTI_EDIT_SENTINEL) {
                const auto &firstNote = selectedAccounts.empty() ? std::string() : selectedAccounts.front()->note;
                const bool allSame = std::ranges::all_of(selectedAccounts, [&firstNote](const auto *acc) {
                    return acc->note == firstNote;
                });

                copyInto(g_editNote.buffer, allSame ? std::string_view(firstNote) : std::string_view());

                g_editNote.accountId = MULTI_EDIT_SENTINEL;
            }

            ImGui::PushItemWidth(ImGui::GetFontSize() * 15.625f);
            ImGui::InputTextMultiline(
                "##EditNoteInput",
                g_editNote.buffer,
                sizeof(g_editNote.buffer),
                ImVec2(0, ImGui::GetTextLineHeight() * 4)
            );
            ImGui::PopItemWidth();

            if (ImGui::Button("Save All##Note")) {
                for (auto *acc: selectedAccounts) {
                    acc->note = g_editNote.buffer;
                }
                Data::SaveAccounts();
                g_editNote.accountId = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndMenu();
        }

        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, getStatusColor("Banned"));
        if (ImGui::MenuItem("Clear Note")) {
            for (auto *acc: selectedAccounts) {
                acc->note.clear();
            }
            Data::SaveAccounts();
        }
        ImGui::PopStyleColor();
    }

    void renderBrowserMenuSingle(const AccountData &account) {
        const auto open = [&account](std::string_view url) {
            if (!account.cookie.empty()) {
                LaunchWebview(std::string(url), account);
            }
        };

        if (ImGui::MenuItem("Home Page")) {
            open("https://www.roblox.com/home");
        }
        if (ImGui::MenuItem("Settings")) {
            open("https://www.roblox.com/my/account");
        }
        if (ImGui::MenuItem("Profile")) {
            open(std::format("https://www.roblox.com/users/{}/profile", account.userId));
        }
        if (ImGui::MenuItem("Messages")) {
            open("https://www.roblox.com/my/messages");
        }
        if (ImGui::MenuItem("Friends")) {
            open("https://www.roblox.com/users/friends");
        }
        if (ImGui::MenuItem("Avatar")) {
            open("https://www.roblox.com/my/avatar");
        }
        if (ImGui::MenuItem("Inventory")) {
            open(std::format("https://www.roblox.com/users/{}/inventory", account.userId));
        }
        if (ImGui::MenuItem("Favorites")) {
            open(std::format("https://www.roblox.com/users/{}/favorites", account.userId));
        }
        if (ImGui::MenuItem("Trades")) {
            open("https://www.roblox.com/trades");
        }
        if (ImGui::MenuItem("Transactions")) {
            open("https://www.roblox.com/transactions");
        }
        if (ImGui::MenuItem("Groups")) {
            open("https://www.roblox.com/communities");
        }
        if (ImGui::MenuItem("Catalog")) {
            open("https://www.roblox.com/catalog");
        }
        if (ImGui::MenuItem("Creator Hub")) {
            open("https://create.roblox.com/dashboard/creations");
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Custom URL")) {
            g_customUrl.open = true;
            g_customUrl.accountId = account.id;
            g_customUrl.buffer[0] = '\0';
        }
    }

    void renderBrowserMenuMulti(const std::vector<const AccountData *> &selectedAccounts, int anchorId) {
        const auto openMany = [&selectedAccounts](std::string_view url) {
            const int eligible = std::ranges::count_if(selectedAccounts, [](const auto *acc) {
                return !acc->cookie.empty();
            });

            const auto launchAll = [selectedAccounts, urlCopy = std::string(url)]() {
                for (const auto *acc: selectedAccounts) {
                    if (!acc->cookie.empty()) {
                        LaunchWebview(urlCopy, *acc);
                    }
                }
            };

            if (eligible >= WEBVIEW_CONFIRM_THRESHOLD) {
                ModalPopup::AddYesNo(std::format("Open {} webviews?", eligible), launchAll);
            } else {
                launchAll();
            }
        };

        const auto openManyPersonal = [&selectedAccounts](auto urlGenerator) {
            const int eligible = std::ranges::count_if(selectedAccounts, [](const auto *acc) {
                return !acc->cookie.empty();
            });

            const auto launchAll = [selectedAccounts, urlGenerator]() {
                for (const auto *acc: selectedAccounts) {
                    if (!acc->cookie.empty()) {
                        LaunchWebview(urlGenerator(*acc), *acc);
                    }
                }
            };

            if (eligible >= WEBVIEW_CONFIRM_THRESHOLD) {
                ModalPopup::AddYesNo("Open webviews?", launchAll);
            } else {
                launchAll();
            }
        };

        if (ImGui::MenuItem("Home Page")) {
            openMany("https://www.roblox.com/home");
        }
        if (ImGui::MenuItem("Settings")) {
            openMany("https://www.roblox.com/my/account");
        }
        if (ImGui::MenuItem("Profile")) {
            openManyPersonal([](const AccountData &acc) {
                return std::format("https://www.roblox.com/users/{}/profile", acc.userId);
            });
        }
        if (ImGui::MenuItem("Messages")) {
            openMany("https://www.roblox.com/my/messages");
        }
        if (ImGui::MenuItem("Friends")) {
            openMany("https://www.roblox.com/users/friends");
        }
        if (ImGui::MenuItem("Avatar")) {
            openMany("https://www.roblox.com/my/avatar");
        }
        if (ImGui::MenuItem("Inventory")) {
            openManyPersonal([](const AccountData &acc) {
                return std::format("https://www.roblox.com/users/{}/inventory", acc.userId);
            });
        }
        if (ImGui::MenuItem("Favorites")) {
            openManyPersonal([](const AccountData &acc) {
                return std::format("https://www.roblox.com/users/{}/favorites", acc.userId);
            });
        }
        if (ImGui::MenuItem("Trades")) {
            openMany("https://www.roblox.com/trades");
        }
        if (ImGui::MenuItem("Transactions")) {
            openMany("https://www.roblox.com/transactions");
        }
        if (ImGui::MenuItem("Groups")) {
            openMany("https://www.roblox.com/communities");
        }
        if (ImGui::MenuItem("Catalog")) {
            openMany("https://www.roblox.com/catalog");
        }
        if (ImGui::MenuItem("Creator Hub")) {
            openMany("https://create.roblox.com/dashboard/creations");
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Custom URL")) {
            g_multiUrl.open = true;
            g_multiUrl.anchorAccountId = anchorId;
            g_multiUrl.buffer[0] = '\0';
        }
    }

    void renderInGameMenuSingle(const AccountData &account) {
        const uint64_t placeId = account.placeId;
        const std::string jobId = account.jobId;

        if (!placeId) {
            ImGui::Separator();
            ImGui::TextDisabled("Fetching server info...");
            return;
        }

        ImGui::Separator();
        StandardJoinMenuParams menu {};
        menu.placeId = placeId;
        menu.jobId = jobId;

        menu.onLaunchGame = [placeId, account]() {
            if (!AccountFilters::IsAccountUsable(account)) {
                return;
            }

            ThreadTask::fireAndForget([placeId, account]() {
                launchWithAccounts(LaunchParams::standard(placeId), {account});
            });
        };

        menu.onLaunchInstance = [placeId, jobId, account]() {
            if (jobId.empty()) {
                return;
            }
            if (!AccountFilters::IsAccountUsable(account)) {
                return;
            }

            ThreadTask::fireAndForget([placeId, jobId, account]() {
                launchWithAccounts(LaunchParams::gameJob(placeId, jobId), {account});
            });
        };

        menu.onFillGame = [placeId]() {
            FillJoinOptions(placeId, "");
        };
        menu.onFillInstance = [placeId, jobId]() {
            if (!jobId.empty()) {
                FillJoinOptions(placeId, jobId);
            }
        };

        RenderStandardJoinMenu(menu);
    }

    void renderUrlPopups(const AccountData &account) {
        if (g_customUrl.open && g_customUrl.accountId == account.id) {
            ImGui::OpenPopup(std::format("Custom URL##Acct{}", account.id).c_str());
            g_customUrl.open = false;
        }

        const auto popupName = std::format("Custom URL##Acct{}", account.id);
        if (ImGui::BeginPopupModal(popupName.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            const auto &style = ImGui::GetStyle();
            const float openWidth = ImGui::CalcTextSize("Open").x + style.FramePadding.x * 2.0f;
            const float cancelWidth = ImGui::CalcTextSize("Cancel").x + style.FramePadding.x * 2.0f;
            const float inputWidth
                = std::max(100.0f, ImGui::GetContentRegionAvail().x - openWidth - cancelWidth - style.ItemSpacing.x);

            ImGui::PushItemWidth(inputWidth);
            ImGui::InputTextWithHint("##AcctUrl", "Enter URL", g_customUrl.buffer, sizeof(g_customUrl.buffer));
            ImGui::PopItemWidth();
            ImGui::Spacing();

            if (ImGui::Button("Open", ImVec2(openWidth, 0)) && g_customUrl.buffer[0] != '\0') {
                LaunchWebview(g_customUrl.buffer, account);
                g_customUrl.buffer[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(0, style.ItemSpacing.x);
            if (ImGui::Button("Cancel", ImVec2(cancelWidth, 0))) {
                g_customUrl.buffer[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (g_multiUrl.open && g_multiUrl.anchorAccountId == account.id) {
            ImGui::OpenPopup("Custom URL##Multiple");
            g_multiUrl.open = false;
        }

        if (ImGui::BeginPopupModal("Custom URL##Multiple", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            const auto &style = ImGui::GetStyle();
            const float openWidth = ImGui::CalcTextSize("Open").x + style.FramePadding.x * 2.0f;
            const float cancelWidth = ImGui::CalcTextSize("Cancel").x + style.FramePadding.x * 2.0f;
            const float inputWidth
                = std::max(100.0f, ImGui::GetContentRegionAvail().x - openWidth - cancelWidth - style.ItemSpacing.x);

            ImGui::PushItemWidth(inputWidth);
            ImGui::InputTextWithHint("##MultiUrl", "Enter URL", g_multiUrl.buffer, sizeof(g_multiUrl.buffer));
            ImGui::PopItemWidth();
            ImGui::Spacing();

            if (ImGui::Button("Open", ImVec2(openWidth, 0)) && g_multiUrl.buffer[0] != '\0') {
                for (AccountData *acc: getSelectedAccountsOrderedMutable()) {
                    if (!acc->cookie.empty()) {
                        LaunchWebview(g_multiUrl.buffer, *acc);
                    }
                }
                g_multiUrl.buffer[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(0, style.ItemSpacing.x);
            if (ImGui::Button("Cancel", ImVec2(cancelWidth, 0))) {
                g_multiUrl.buffer[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

} // namespace

void LaunchBrowserWithCookie(const AccountData &account) {
    if (account.cookie.empty()) {
        LOG_WARN("Cannot open browser - cookie is empty for account: " + account.displayName);
        return;
    }
    LOG_INFO("Launching WebView2 browser for account: " + account.displayName);
    LaunchWebview("https://www.roblox.com/home", account);
}

void RenderAccountContextMenu(AccountData &account, const std::string &uniqueContextMenuId) {
    if (!ImGui::BeginPopupContextItem(uniqueContextMenuId.c_str())) {
        renderUrlPopups(account);
        return;
    }

    const bool isMultiSelection = (g_selectedAccountIds.size() > 1) && g_selectedAccountIds.contains(account.id);

    if (ImGui::IsWindowAppearing()) {
        if (account.status == "InGame" && account.placeId == 0 && !account.userId.empty()) {
            asyncFetchPresence(account.id, account.userId, account.cookie);
        }
    }

    if (isMultiSelection) {
        ImGui::TextUnformatted("Multiple Accounts");
        ImGui::Separator();
    } else {
        ImGui::TextUnformatted("Account: ");
        ImGui::SameLine(0, 0);
        const ImVec4 nameColor = getStatusColor(account.status);
        ImGui::PushStyleColor(ImGuiCol_Text, nameColor);
        ImGui::TextUnformatted(account.displayName.empty() ? account.username.c_str() : account.displayName.c_str());
        ImGui::PopStyleColor();

        if (g_selectedAccountIds.contains(account.id)) {
            ImGui::SameLine();
            ImGui::TextDisabled("(Selected)");
        }
        ImGui::Separator();
    }

    if (ImGui::BeginMenu("Copy Info")) {
        if (isMultiSelection) {
            renderCopyInfoMenuMulti(getSelectedAccountsOrdered());
        } else {
            renderCopyInfoMenuSingle(account);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Note")) {
        if (isMultiSelection) {
            auto selectedAccounts = getSelectedAccountsOrderedMutable();
            renderNoteMenuMulti(selectedAccounts);
        } else {
            renderNoteMenuSingle(account);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Browser")) {
        if (isMultiSelection) {
            renderBrowserMenuMulti(getSelectedAccountsOrdered(), account.id);
        } else {
            renderBrowserMenuSingle(account);
        }
        ImGui::EndMenu();
    }

    if (!isMultiSelection && account.status == "InGame") {
        renderInGameMenuSingle(account);
    }

    ImGui::Separator();

    if (!isMultiSelection) {
        if (ImGui::MenuItem("Set as Default Account")) {
            g_defaultAccountId = account.id;
            g_selectedAccountIds.clear();
            g_selectedAccountIds.insert(account.id);
            Data::SaveSettings();
        }
    }

    if (isMultiSelection) {
        const int removeCount = static_cast<int>(g_selectedAccountIds.size());
        ImGui::PushStyleColor(ImGuiCol_Text, getStatusColor("Terminated"));

        if (ImGui::MenuItem(std::format("Remove {} Accounts", removeCount).c_str())) {
            std::vector<int> idsToRemove(g_selectedAccountIds.begin(), g_selectedAccountIds.end());
            ModalPopup::AddYesNo(std::format("Delete {} accounts?", removeCount), [idsToRemove]() {
                const std::unordered_set<int> toRemove(idsToRemove.begin(), idsToRemove.end());

                for (const int id: idsToRemove) {
                    if (const AccountData *acc = getAccountById(id)) {
#ifdef __APPLE__
                        MultiInstance::cleanupUserEnvironment(acc->username);
#endif
                    }
                }

                std::erase_if(g_accounts, [&toRemove](const auto &acc) {
                    return toRemove.contains(acc.id);
                });
                invalidateAccountIndex();

                for (const int id: idsToRemove) {
                    g_selectedAccountIds.erase(id);
                }
                LOG_INFO("Deleted selected accounts");
                Data::SaveAccounts();
            });
        }
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, getStatusColor("Terminated"));
        if (ImGui::MenuItem("Remove Account")) {
            ModalPopup::AddYesNo(
                std::format("Delete {}?", account.displayName),
                [id = account.id, displayName = account.displayName, username = account.username]() {
                    LOG_INFO("Attempting to delete account: {} (ID: {})", displayName, id);
#ifdef __APPLE__
                    if (!MultiInstance::cleanupUserEnvironment(username)) {
                        LOG_WARN("Environment cleanup failed for " + username);
                    }
#endif
                    std::erase_if(g_accounts, [id](const auto &acc) {
                        return acc.id == id;
                    });
                    invalidateAccountIndex();
                    g_selectedAccountIds.erase(id);
                    Data::SaveAccounts();
                    LOG_INFO("Successfully deleted account: {} (ID: {})", displayName, id);
                }
            );
        }
        ImGui::PopStyleColor();
    }

    ImGui::EndPopup();
    renderUrlPopups(account);
}
