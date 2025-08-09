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
                                        a.jobId = it->second.gameId;
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

        // Copy Info submenu
        if (BeginMenu("Copy Info")) {
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

        // Note submenu
        if (BeginMenu("Note")) {
            if (MenuItem("Copy Note")) SetClipboardText(account.note.c_str());
            Separator();
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
                SameLine();
                if (Button("Cancel##Note")) {
                    g_editing_note_for_account_id_ctx = -1;
                    CloseCurrentPopup();
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }


        // Browser submenu (moved above in-game section)
        if (BeginMenu("Browser")) {
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

    // In-game section
    if (account.status == "InGame") {
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

        // Set default account
        if (MenuItem("Set as Default Account")) {
            g_defaultAccountId = account.id;
            g_selectedAccountIds.clear();
            g_selectedAccountIds.insert(account.id);
            Data::SaveSettings("settings.json");
        }

    PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
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
}
