#define CRT_SECURE_NO_WARNINGS
#include "accounts_context_menu.h"
#include <shlobj_core.h>
#include <imgui.h>
#include <chrono>
#include <random>
#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <wrl.h>
#include <wil/com.h>
#include <thread>
#include <atomic>
#include <dwmapi.h>
#include <memory>
#include <algorithm>

#include "network/roblox.h"
#include "ui/webview.hpp"
#include "../webview_helpers.h"
#include "system/threading.h"
#include "system/launcher.hpp"
#include "core/logging.hpp"
#include "core/status.h"
#include "ui/confirm.h"
#include "../../ui.h"
#include "../data.h"
#include "accounts_join_ui.h"
#include "../context_menus.h"
#include "../../utils/core/account_utils.h"
#include "../../utils/network/roblox/common.h"

#pragma comment(lib, "Dwmapi.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

static char g_edit_note_buffer_ctx[1024];
static int g_editing_note_for_account_id_ctx = -1;

static bool g_openCustomUrlPopup = false;
static int g_customUrlAccountId = -1;
static char g_customUrlBuffer[256] = "";

// Multi-selection custom URL popup state
static bool g_openMultiCustomUrlPopup = false;
static int g_multiCustomUrlAnchorId = -1;
static char g_multiCustomUrlBuffer[256] = "";

// Deprecated local cache: use AccountData cached fields populated by background refresh

using namespace ImGui;
using namespace std;

template <typename Container, typename Pred>
static inline void erase_if_local(Container &c, Pred p) {
    c.erase(remove_if(c.begin(), c.end(), p), c.end());
}

void LaunchBrowserWithCookie(const AccountData &account) {
    if (account.cookie.empty()) {
        LOG_WARN("Cannot open browser - cookie is empty for account: " + account.displayName);
        return;
    }

    LOG_INFO("Launching WebView2 browser for account: " + account.displayName);

    LaunchWebview("https://www.roblox.com/home", account);
}

static std::unordered_set<int> g_presenceFetchInFlight;

void RenderAccountContextMenu(AccountData &account, const string &unique_context_menu_id) {
    // No-op: rely on AccountData cached fields

    if (BeginPopupContextItem(unique_context_menu_id.c_str())) {
        bool isMultiSelectionContext = (g_selectedAccountIds.size() > 1) && (g_selectedAccountIds.find(account.id) != g_selectedAccountIds.end());
        // If user is InGame but cached placeId/jobId are missing, kick off a non-blocking fetch once
        if (IsWindowAppearing()) {
            if (account.status == "InGame" && account.placeId == 0 && !account.userId.empty()) {
                if (g_presenceFetchInFlight.find(account.id) == g_presenceFetchInFlight.end()) {
                    g_presenceFetchInFlight.insert(account.id);
                    Threading::newThread([acctId = account.id, userIdStr = account.userId, cookie = account.cookie]() {
                        try {
                            uint64_t uid = stoull(userIdStr);
                            auto pres = Roblox::getPresences({uid}, cookie);
                            auto it = pres.find(uid);
                            if (it != pres.end()) {
                                for (auto &a : g_accounts) {
                                    if (a.id == acctId) {
                                        a.placeId = it->second.placeId;
                                        a.jobId = it->second.jobId;
                                        break;
                                    }
                                }
                            }
                        } catch (...) {
                        }
                        g_presenceFetchInFlight.erase(acctId);
                    });
                }
            }
        }

        if (isMultiSelectionContext) {
            TextUnformatted("Multiple Accounts");
            Separator();
        } else {
            TextUnformatted("Account: ");
            SameLine(0, 0);
            ImVec4 nameCol = getStatusColor(account.status);
            PushStyleColor(ImGuiCol_Text, nameCol);
            TextUnformatted(account.displayName.empty() ? account.username.c_str() : account.displayName.c_str());
            PopStyleColor();
            if (g_selectedAccountIds.find(account.id) != g_selectedAccountIds.end()) {
                SameLine();
                TextDisabled("(Selected)");
            }
            Separator();
        }

        // Copy Info submenu
        if (BeginMenu("Copy Info")) {
            if (isMultiSelectionContext) {
                // Build ordered selection list based on g_accounts order
                vector<const AccountData*> selectedAccounts;
                selectedAccounts.reserve(g_selectedAccountIds.size());
                for (const auto &a : g_accounts) {
                    if (g_selectedAccountIds.find(a.id) != g_selectedAccountIds.end()) selectedAccounts.push_back(&a);
                }
                auto joinField = [&](auto getter) {
                    string out;
                    for (const AccountData* ap : selectedAccounts) {
                        string v = getter(*ap);
                        if (!out.empty()) out += "\n";
                        out += v;
                    }
                    return out;
                };
                if (MenuItem("Display Name")) {
                    auto s = joinField([](const AccountData &a) { return a.displayName; });
                    SetClipboardText(s.c_str());
                }
                if (MenuItem("Username")) {
                    auto s = joinField([](const AccountData &a) { return a.username; });
                    SetClipboardText(s.c_str());
                }
                if (MenuItem("User ID")) {
                    auto s = joinField([](const AccountData &a) { return a.userId; });
                    SetClipboardText(s.c_str());
                }
                Separator();
                bool anyCookie = any_of(selectedAccounts.begin(), selectedAccounts.end(), [](const AccountData* ap){ return !ap->cookie.empty(); });
                {
                    PushStyleColor(ImGuiCol_Text, getStatusColor("Warned"));
                    bool clicked = MenuItem("Cookie", nullptr, false, anyCookie);
                    PopStyleColor();
                    if (clicked) {
                        string s;
                        for (const AccountData* ap : selectedAccounts) {
                            if (ap->cookie.empty()) continue;
                            if (!s.empty()) s += "\n";
                            s += ap->cookie;
                        }
                        if (!s.empty()) SetClipboardText(s.c_str());
                    }
                }
                {
                    PushStyleColor(ImGuiCol_Text, getStatusColor("Warned"));
                    bool clicked = MenuItem("Launch Link", nullptr, false, anyCookie);
                    PopStyleColor();
                    if (clicked) {
                        vector<pair<int,string>> accs;
                        for (const AccountData* ap : selectedAccounts) if (!ap->cookie.empty()) accs.emplace_back(ap->id, ap->cookie);
                        string place_id_str = join_value_buf;
                        string job_id_str = join_jobid_buf;
                        Threading::newThread([accs, place_id_str, job_id_str]() {
                            bool hasJob = !job_id_str.empty();
                            auto now_ms = chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch()).count();
                            thread_local mt19937_64 rng{random_device{}()};
                            static uniform_int_distribution<int> d1(100000, 130000), d2(100000, 900000);
                            string out;
                            for (auto &p : accs) {
                                string ticket = Roblox::fetchAuthTicket(p.second);
                                if (ticket.empty()) continue;
                                string browserTracker = to_string(d1(rng)) + to_string(d2(rng));
                                string placeLauncherUrl =
                                    "https://assetgame.roblox.com/game/PlaceLauncher.ashx?request=RequestGame%26placeId=" + place_id_str;
                                if (hasJob) { placeLauncherUrl += "%26gameId=" + job_id_str; }
                                string uri = string("roblox-player://1/1+launchmode:play")
                                    + "+gameinfo:" + ticket
                                    + "+launchtime:" + to_string(now_ms)
                                    + "+browsertrackerid:" + browserTracker
                                    + "+placelauncherurl:" + placeLauncherUrl
                                    + "+robloxLocale:en_us+gameLocale:en_us";
                                if (!out.empty()) out += "\n";
                                out += uri;
                            }
                            if (!out.empty()) SetClipboardText(out.c_str());
                        });
                    }
                }
                ImGui::EndMenu();
            } else {
                if (MenuItem("Display Name")) SetClipboardText(account.displayName.c_str());
                if (MenuItem("Username")) SetClipboardText(account.username.c_str());
                if (MenuItem("User ID")) SetClipboardText(account.userId.c_str());
                Separator();
                {
                    PushStyleColor(ImGuiCol_Text, getStatusColor("Warned"));
                    bool clicked = MenuItem("Cookie", nullptr, false, !account.cookie.empty());
                    PopStyleColor();
                    if (clicked) {
                        SetClipboardText(account.cookie.c_str());
                    }
                }
                {
                    PushStyleColor(ImGuiCol_Text, getStatusColor("Warned"));
                    bool clicked = MenuItem("Launch Link", nullptr, false, !account.cookie.empty());
                    PopStyleColor();
                    if (clicked) {
                        string acc_cookie = account.cookie;
                        string place_id_str = join_value_buf;
                        string job_id_str = join_jobid_buf;
                        Threading::newThread(
                            [acc_cookie, place_id_str, job_id_str, account_id = account.id, account_display_name = account.displayName] {
                                bool hasJob = !job_id_str.empty();
                                auto now_ms = chrono::duration_cast<chrono::milliseconds>(
                                    chrono::system_clock::now().time_since_epoch()).count();
                                thread_local mt19937_64 rng{random_device{}()};
                                static uniform_int_distribution<int> d1(100000, 130000), d2(100000, 900000);
                                string browserTracker = to_string(d1(rng)) + to_string(d2(rng));
                                string ticket = Roblox::fetchAuthTicket(acc_cookie);
                                if (ticket.empty()) return;
                                string placeLauncherUrl =
                                        "https://assetgame.roblox.com/game/PlaceLauncher.ashx?request=RequestGame%26placeId="
                                        + place_id_str;
                                if (hasJob) { placeLauncherUrl += "%26gameId=" + job_id_str; }
                                string uri =
                                        string("roblox-player://1/1+launchmode:play")
                                        + "+gameinfo:" + ticket
                                        + "+launchtime:" + to_string(now_ms)
                                        + "+browsertrackerid:" + browserTracker
                                        + "+placelauncherurl:" + placeLauncherUrl
                                        + "+robloxLocale:en_us+gameLocale:en_us";
                                SetClipboardText(uri.c_str());
                            });
                    }
                }
                ImGui::EndMenu();
            }
        }

        // Note submenu (supports single and multi selection)
        if (BeginMenu("Note")) {
            if (isMultiSelectionContext) {
                // Build ordered selection list based on g_accounts order
                vector<AccountData*> selectedAccounts;
                selectedAccounts.reserve(g_selectedAccountIds.size());
                for (auto &a : g_accounts) {
                    if (g_selectedAccountIds.find(a.id) != g_selectedAccountIds.end()) selectedAccounts.push_back(&a);
                }

                if (MenuItem("Copy Note")) {
                    string out;
                    for (const AccountData* ap : selectedAccounts) {
                        if (!out.empty()) out += "\n";
                        out += ap->note;
                    }
                    SetClipboardText(out.c_str());
                }
                if (BeginMenu("Edit Note")) {
                    // Initialize edit buffer once per open for multi-edit using sentinel id -2
                    if (g_editing_note_for_account_id_ctx != -2) {
                        string firstNote = selectedAccounts.empty() ? string() : selectedAccounts.front()->note;
                        bool allSame = true;
                        for (const AccountData* ap : selectedAccounts) {
                            if (ap->note != firstNote) { allSame = false; break; }
                        }
                        const string &initial = allSame ? firstNote : string();
                        strncpy_s(g_edit_note_buffer_ctx, initial.c_str(), sizeof(g_edit_note_buffer_ctx) - 1);
                        g_edit_note_buffer_ctx[sizeof(g_edit_note_buffer_ctx) - 1] = '\0';
                        g_editing_note_for_account_id_ctx = -2;
                    }
                    PushItemWidth(GetFontSize() * 15.625f);
                    InputTextMultiline("##EditNoteInput", g_edit_note_buffer_ctx, sizeof(g_edit_note_buffer_ctx), ImVec2(0, GetTextLineHeight() * 4));
                    PopItemWidth();
                    if (Button("Save All##Note")) {
                        for (auto *ap : selectedAccounts) {
                            ap->note = g_edit_note_buffer_ctx;
                        }
                        Data::SaveAccounts();
                        g_editing_note_for_account_id_ctx = -1;
                        CloseCurrentPopup();
                    }
                    ImGui::EndMenu();
                }
                Separator();
                {
                    PushStyleColor(ImGuiCol_Text, getStatusColor("Banned"));
                    if (MenuItem("Clear Note")) {
                        for (auto *ap : selectedAccounts) { ap->note.clear(); }
                        Data::SaveAccounts();
                    }
                    PopStyleColor();
                }
                ImGui::EndMenu();
            } else {
                if (MenuItem("Copy Note")) SetClipboardText(account.note.c_str());
                if (BeginMenu("Edit Note")) {
                    if (g_editing_note_for_account_id_ctx != account.id) {
                        strncpy_s(g_edit_note_buffer_ctx, account.note.c_str(), sizeof(g_edit_note_buffer_ctx) - 1);
                        g_edit_note_buffer_ctx[sizeof(g_edit_note_buffer_ctx) - 1] = '\0';
                        g_editing_note_for_account_id_ctx = account.id;
                    }
                    PushItemWidth(GetFontSize() * 15.625f);
                    InputTextMultiline("##EditNoteInput", g_edit_note_buffer_ctx, sizeof(g_edit_note_buffer_ctx), ImVec2(0, GetTextLineHeight() * 4));
                    PopItemWidth();
                    if (Button("Save##Note")) {
                        if (g_editing_note_for_account_id_ctx == account.id) {
                            account.note = g_edit_note_buffer_ctx;
                            Data::SaveAccounts();
                        }
                        g_editing_note_for_account_id_ctx = -1;
                        CloseCurrentPopup();
                    }
                    ImGui::EndMenu();
                }
                Separator();
                {
                    PushStyleColor(ImGuiCol_Text, getStatusColor("Banned"));
                    if (MenuItem("Clear Note")) {
                        account.note.clear();
                        Data::SaveAccounts();
                    }
                    PopStyleColor();
                }
                ImGui::EndMenu();
            }
        }


        // Browser submenu (moved above in-game section)
        if (BeginMenu("Browser")) {
            if (isMultiSelectionContext) {
                // Build ordered selection list
                vector<const AccountData*> selectedAccounts;
                selectedAccounts.reserve(g_selectedAccountIds.size());
                for (const auto &a : g_accounts) {
                    if (g_selectedAccountIds.find(a.id) != g_selectedAccountIds.end()) selectedAccounts.push_back(&a);
                }
                auto openMany = [&](const string &url) {
                    int countEligible = 0;
                    for (const AccountData* ap : selectedAccounts) if (!ap->cookie.empty()) ++countEligible;
                    auto launchAll = [selectedAccounts, url]() {
                        for (const AccountData* ap : selectedAccounts) {
                            if (!ap->cookie.empty()) LaunchWebview(url, *ap);
                        }
                    };
                    if (countEligible >= 3) {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "Open %d webviews?", countEligible);
                        ConfirmPopup::Add(buf, launchAll);
                    } else {
                        launchAll();
                    }
                };
                if (MenuItem("Home Page")) openMany("https://www.roblox.com/home");
                if (MenuItem("Settings")) openMany("https://www.roblox.com/my/account");
                if (MenuItem("Profile")) {
                    // per-account URLs; open individually
                    int countEligible = 0;
                    for (const AccountData* ap : selectedAccounts) if (!ap->cookie.empty()) ++countEligible;
                    auto launchAll = [selectedAccounts]() {
                        for (const AccountData* ap : selectedAccounts) {
                            if (!ap->cookie.empty()) LaunchWebview("https://www.roblox.com/users/" + ap->userId + "/profile", *ap);
                        }
                    };
                    if (countEligible >= 3) { ConfirmPopup::Add("Open profile webviews?", launchAll); } else { launchAll(); }
                }
                if (MenuItem("Messages")) openMany("https://www.roblox.com/my/messages");
                if (MenuItem("Friends")) openMany("https://www.roblox.com/users/friends");
                if (MenuItem("Avatar")) openMany("https://www.roblox.com/my/avatar");
                if (MenuItem("Inventory")) {
                    int countEligible = 0;
                    for (const AccountData* ap : selectedAccounts) if (!ap->cookie.empty()) ++countEligible;
                    auto launchAll = [selectedAccounts]() {
                        for (const AccountData* ap : selectedAccounts) {
                            if (!ap->cookie.empty()) LaunchWebview("https://www.roblox.com/users/" + ap->userId + "/inventory", *ap);
                        }
                    };
                    if (countEligible >= 3) { ConfirmPopup::Add("Open inventory webviews?", launchAll); } else { launchAll(); }
                }
                if (MenuItem("Favorites")) {
                    int countEligible = 0;
                    for (const AccountData* ap : selectedAccounts) if (!ap->cookie.empty()) ++countEligible;
                    auto launchAll = [selectedAccounts]() {
                        for (const AccountData* ap : selectedAccounts) {
                            if (!ap->cookie.empty()) LaunchWebview("https://www.roblox.com/users/" + ap->userId + "/favorites", *ap);
                        }
                    };
                    if (countEligible >= 3) { ConfirmPopup::Add("Open favorites webviews?", launchAll); } else { launchAll(); }
                }
                if (MenuItem("Trades")) openMany("https://www.roblox.com/trades");
                if (MenuItem("Transactions")) openMany("https://www.roblox.com/transactions");
                if (MenuItem("Groups")) openMany("https://www.roblox.com/communities");
                if (MenuItem("Catalog")) openMany("https://www.roblox.com/catalog");
                if (MenuItem("Creator Hub")) openMany("https://create.roblox.com/dashboard/creations");
                Separator();
                if (MenuItem("Custom URL")) {
                    g_openMultiCustomUrlPopup = true;
                    g_multiCustomUrlAnchorId = account.id;
                    g_multiCustomUrlBuffer[0] = '\0';
                }
                ImGui::EndMenu();
            } else {
                auto open = [&](const string &url) {
                    if (!account.cookie.empty()) LaunchWebview(url, account);
                };
                if (MenuItem("Home Page")) open("https://www.roblox.com/home");
                if (MenuItem("Settings")) open("https://www.roblox.com/my/account");
                if (MenuItem("Profile")) open("https://www.roblox.com/users/" + account.userId + "/profile");
                if (MenuItem("Messages")) open("https://www.roblox.com/my/messages");
                if (MenuItem("Friends")) open("https://www.roblox.com/users/friends");
                if (MenuItem("Avatar")) open("https://www.roblox.com/my/avatar");
                if (MenuItem("Inventory")) open("https://www.roblox.com/users/" + account.userId + "/inventory");
                if (MenuItem("Favorites")) open("https://www.roblox.com/users/" + account.userId + "/favorites");
                if (MenuItem("Trades")) open("https://www.roblox.com/trades");
                if (MenuItem("Transactions")) open("https://www.roblox.com/transactions");
                if (MenuItem("Groups")) open("https://www.roblox.com/communities");
                if (MenuItem("Catalog")) open("https://www.roblox.com/catalog");
                if (MenuItem("Creator Hub")) open("https://create.roblox.com/dashboard/creations");
                Separator();
                if (MenuItem("Custom URL")) {
                    g_openCustomUrlPopup = true;
                    g_customUrlAccountId = account.id;
                    g_customUrlBuffer[0] = '\0';
                }
                ImGui::EndMenu();
            }
        }

    // In-game section (single-account only)
    if (!isMultiSelectionContext && account.status == "InGame") {
            uint64_t placeId = account.placeId;
            string jobId = account.jobId;
            if (placeId) {
        Separator();
                StandardJoinMenuParams menu{};
                menu.placeId = placeId;
                menu.jobId = jobId;
                menu.onLaunchGame = [pid = placeId, &account]() {
                    vector<pair<int, string>> accounts;
                    if (AccountFilters::IsAccountUsable(account)) accounts.emplace_back(account.id, account.cookie);
                    if (!accounts.empty()) Threading::newThread([pid, accounts]() { launchRobloxSequential(pid, "", accounts); });
                };
                menu.onLaunchInstance = [pid = placeId, jid = jobId, &account]() {
                    if (jid.empty()) return;
                    vector<pair<int, string>> accounts;
                    if (AccountFilters::IsAccountUsable(account)) accounts.emplace_back(account.id, account.cookie);
                    if (!accounts.empty()) Threading::newThread([pid, jid, accounts]() { launchRobloxSequential(pid, jid, accounts); });
                };
                menu.onFillGame = [pid = placeId]() { FillJoinOptions(pid, ""); };
                menu.onFillInstance = [pid = placeId, jid = jobId]() { if (!jid.empty()) FillJoinOptions(pid, jid); };
                RenderStandardJoinMenu(menu);
            } else {
                Separator();
                TextDisabled("Fetching server info...");
            }
        }
        Separator();

        // Set default account (single-account only)
        if (!isMultiSelectionContext) {
            if (MenuItem("Set as Default Account")) {
                g_defaultAccountId = account.id;
                g_selectedAccountIds.clear();
                g_selectedAccountIds.insert(account.id);
                Data::SaveSettings("settings.json");
            }
        }

        // Removal
        if (isMultiSelectionContext) {
            int removeCount = static_cast<int>(g_selectedAccountIds.size());
            PushStyleColor(ImGuiCol_Text, getStatusColor("Terminated"));
            char label[64];
            snprintf(label, sizeof(label), "Remove %d Accounts", removeCount);
            if (MenuItem(label)) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Delete %d accounts?", removeCount);
                // Capture ids to remove
                vector<int> ids;
                ids.reserve(g_selectedAccountIds.size());
                for (int idSel : g_selectedAccountIds) ids.push_back(idSel);
                ConfirmPopup::Add(buf, [ids]() {
                    unordered_set<int> toRemove(ids.begin(), ids.end());
                    erase_if_local(g_accounts, [&](const AccountData &acc_data) { return toRemove.find(acc_data.id) != toRemove.end(); });
                    for (int id : ids) g_selectedAccountIds.erase(id);
                    Status::Set("Deleted selected accounts");
                    Data::SaveAccounts();
                });
            }
            PopStyleColor();
        } else {
            PushStyleColor(ImGuiCol_Text, getStatusColor("Terminated"));
            if (MenuItem("Remove Account")) {
                char buf[256];
                snprintf(buf, sizeof(buf), "Delete %s?", account.displayName.c_str());
                ConfirmPopup::Add(buf, [id = account.id, displayName = account.displayName]() {
                    LOG_INFO("Attempting to delete account: " + displayName + " (ID: " + to_string(id) + ")");
                    erase_if_local(g_accounts, [&](const AccountData &acc_data) { return acc_data.id == id; });
                    g_selectedAccountIds.erase(id);
                    Status::Set("Deleted account " + displayName);
                    Data::SaveAccounts();
                    LOG_INFO("Successfully deleted account: " + displayName + " (ID: " + to_string(id) + ")");
                });
            }
            PopStyleColor();
        }
        EndPopup();
    }

    if (g_openCustomUrlPopup && g_customUrlAccountId == account.id) {
        string popupId = "Custom URL##Acct" + to_string(account.id);
        OpenPopup(popupId.c_str());
        g_openCustomUrlPopup = false;
    }
    string popupName = "Custom URL##Acct" + to_string(account.id);
    if (BeginPopupModal(popupName.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGuiStyle &style = GetStyle();
        float openWidth = CalcTextSize("Open").x + style.FramePadding.x * 2.0f;
        float cancelWidth = CalcTextSize("Cancel").x + style.FramePadding.x * 2.0f;
        float inputWidth = GetContentRegionAvail().x - openWidth - cancelWidth - style.ItemSpacing.x;
        if (inputWidth < 100.0f)
            inputWidth = 100.0f;
        PushItemWidth(inputWidth);
        InputTextWithHint("##AcctUrl", "Enter URL", g_customUrlBuffer, sizeof(g_customUrlBuffer));
        PopItemWidth();
        Spacing();
        if (Button("Open", ImVec2(openWidth, 0)) && g_customUrlBuffer[0] != '\0') {
            LaunchWebview(g_customUrlBuffer, account);
            g_customUrlBuffer[0] = '\0';
            CloseCurrentPopup();
        }
        SameLine(0, style.ItemSpacing.x);
        if (Button("Cancel", ImVec2(cancelWidth, 0))) {
            g_customUrlBuffer[0] = '\0';
            CloseCurrentPopup();
        }
        EndPopup();
    }

    // Multi-selection custom URL modal
    if (g_openMultiCustomUrlPopup && g_multiCustomUrlAnchorId == account.id) {
        OpenPopup("Custom URL##Multiple");
        g_openMultiCustomUrlPopup = false;
    }
    if (BeginPopupModal("Custom URL##Multiple", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGuiStyle &style = GetStyle();
        float openWidth = CalcTextSize("Open").x + style.FramePadding.x * 2.0f;
        float cancelWidth = CalcTextSize("Cancel").x + style.FramePadding.x * 2.0f;
        float inputWidth = GetContentRegionAvail().x - openWidth - cancelWidth - style.ItemSpacing.x;
        if (inputWidth < 100.0f)
            inputWidth = 100.0f;
        PushItemWidth(inputWidth);
        InputTextWithHint("##MultiUrl", "Enter URL", g_multiCustomUrlBuffer, sizeof(g_multiCustomUrlBuffer));
        PopItemWidth();
        Spacing();
        if (Button("Open", ImVec2(openWidth, 0)) && g_multiCustomUrlBuffer[0] != '\0') {
            // Open for all selected accounts that have a cookie
            for (auto &a : g_accounts) {
                if (g_selectedAccountIds.find(a.id) != g_selectedAccountIds.end() && !a.cookie.empty()) {
                    LaunchWebview(g_multiCustomUrlBuffer, a);
                }
            }
            g_multiCustomUrlBuffer[0] = '\0';
            CloseCurrentPopup();
        }
        SameLine(0, style.ItemSpacing.x);
        if (Button("Cancel", ImVec2(cancelWidth, 0))) {
            g_multiCustomUrlBuffer[0] = '\0';
            CloseCurrentPopup();
        }
        EndPopup();
    }
}
