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
#include <wrl.h>
#include <wil/com.h>
#include <thread>
#include <atomic>
#include <dwmapi.h>
#include <memory>

#include "../../utils/network/roblox_api.h"
#include "../../utils/network/webview.hpp"
#include "../../utils/threads/threading.h"
#include "../../utils/logging/logging.hpp"
#include "../../utils/gui/status.h"
#include "../../utils/gui/confirm.h"
#include "../../ui.h"
#include "../data.h"

#pragma comment(lib, "Dwmapi.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

static char g_edit_note_buffer_ctx[1024];
static int g_editing_note_for_account_id_ctx = -1;

static bool g_openCustomUrlPopup = false;
static int g_customUrlAccountId = -1;
static char g_customUrlBuffer[256] = "";

// Cache game information for accounts when context menus are opened so we
// don't repeatedly hit the network every frame.
struct CachedGameInfo {
    uint64_t placeId = 0;
    std::string jobId;
};

static std::unordered_map<int, CachedGameInfo> g_cachedGameInfo;

using namespace ImGui;
using namespace std;

void LaunchBrowserWithCookie(const AccountData &account) {
    if (account.cookie.empty()) {
        LOG_WARN("Cannot open browser - cookie is empty for account: " + account.displayName);
        return;
    }

    LOG_INFO("Launching WebView2 browser for account: " + account.displayName);

    LaunchWebview("https://www.roblox.com/home", account.username + " - " + account.userId,
                  account.cookie);
}

void RenderAccountContextMenu(AccountData &account, const string &unique_context_menu_id) {
    if (!IsPopupOpen(unique_context_menu_id.c_str()))
        g_cachedGameInfo.erase(account.id);

    if (BeginPopupContextItem(unique_context_menu_id.c_str())) {
        if (IsWindowAppearing()) {
            // Refresh cached game data when the menu is opened
            g_cachedGameInfo.erase(account.id);
            if (account.status == "InGame") {
                try {
                    auto pres = RobloxApi::getPresences({stoull(account.userId)}, account.cookie);
                    auto itp = pres.find(stoull(account.userId));
                    if (itp != pres.end()) {
                        g_cachedGameInfo[account.id] = {itp->second.placeId, itp->second.gameId};
                    }
                } catch (...) {
                }
            }
        }

        Text("Account: %s", account.displayName.c_str());
        if (g_selectedAccountIds.contains(account.id)) {
            SameLine();
            TextDisabled("(Selected)");
        }
        Separator();

        if (BeginMenu("Edit Note")) {
            if (g_editing_note_for_account_id_ctx != account.id) {
                strncpy_s(g_edit_note_buffer_ctx, account.note.c_str(), sizeof(g_edit_note_buffer_ctx) - 1);
                g_edit_note_buffer_ctx[sizeof(g_edit_note_buffer_ctx) - 1] = '\0';
                g_editing_note_for_account_id_ctx = account.id;
            }

            PushItemWidth(250.0f);
            InputTextMultiline("##EditNoteInput", g_edit_note_buffer_ctx, sizeof(g_edit_note_buffer_ctx),
                               ImVec2(0, GetTextLineHeight() * 4));
            PopItemWidth();

            if (Button("Save##Note")) {
                if (g_editing_note_for_account_id_ctx == account.id) {
                    account.note = g_edit_note_buffer_ctx;
                    Data::SaveAccounts();
                    printf("Note updated for account ID %d: %s\n", account.id, account.note.c_str());
                    LOG_INFO("Note updated for account ID " + to_string(account.id) + ": " + account.note);
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

        Separator();


        if (account.status == "InGame") {
            uint64_t placeId = 0;
            string jobId;
            auto itCache = g_cachedGameInfo.find(account.id);
            if (itCache != g_cachedGameInfo.end()) {
                placeId = itCache->second.placeId;
                jobId = itCache->second.jobId;
            }

            if (placeId && !jobId.empty()) {
                if (MenuItem("Copy Place ID"))
                    SetClipboardText(to_string(placeId).c_str());
                if (MenuItem("Copy Job ID"))
                    SetClipboardText(jobId.c_str());
                if (BeginMenu("Copy Launch Method")) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "roblox://placeId=%llu&gameInstanceId=%s", (unsigned long long) placeId,
                             jobId.c_str());
                    if (MenuItem("Deep Link")) SetClipboardText(buf);
                    string js = "Roblox.GameLauncher.joinGameInstance(" + to_string(placeId) + ", \"" + jobId + "\")";
                    if (MenuItem("JavaScript")) SetClipboardText(js.c_str());
                    string luau = "game:GetService(\"TeleportService\"):TeleportToPlaceInstance(" + to_string(placeId) +
                                  ", \"" + jobId + "\")";
                    if (MenuItem("ROBLOX Luau")) SetClipboardText(luau.c_str());
                    ImGui::EndMenu();
                }
                if (MenuItem("Generate Invite Link")) {
                    string link = "https://www.roblox.com/games/start?placeId=" + to_string(placeId) +
                                  "&gameInstanceId=" + jobId;
                    SetClipboardText(link.c_str());
                }
                Separator();
            }
        }

        if (BeginMenu("Open In Browser")) {
            if (MenuItem("Home Page")) {
                if (!account.cookie.empty())
                    LaunchWebview("https://www.roblox.com/home", account.username + " - " + account.userId,
                                  account.cookie);
            }
            if (MenuItem("Profile")) {
                if (!account.cookie.empty())
                    LaunchWebview("https://www.roblox.com/users/" + account.userId + "/profile", account.username,
                                  account.cookie);
            }
            if (MenuItem("Avatar")) {
                if (!account.cookie.empty())
                    LaunchWebview("https://www.roblox.com/my/avatar", account.username, account.cookie);
            }
            if (MenuItem("Friends")) {
                if (!account.cookie.empty())
                    LaunchWebview("https://www.roblox.com/users/friends", account.username, account.cookie);
            }
            if (MenuItem("Messages")) {
                if (!account.cookie.empty())
                    LaunchWebview("https://www.roblox.com/my/messages", account.username, account.cookie);
            }
            if (MenuItem("Catalog")) {
                if (!account.cookie.empty())
                    LaunchWebview("https://www.roblox.com/catalog", account.username, account.cookie);
            }
            if (MenuItem("Creator Hub")) {
                if (!account.cookie.empty())
                    LaunchWebview("https://create.roblox.com/", account.username, account.cookie);
            }
            if (MenuItem("Custom URL")) {
                g_openCustomUrlPopup = true;
                g_customUrlAccountId = account.id;
                g_customUrlBuffer[0] = '\0';
            }
            ImGui::EndMenu();
        }

        if (MenuItem("Copy Launch Link")) {
            string acc_cookie = account.cookie;
            string place_id_str = join_value_buf;
            string job_id_str = join_jobid_buf;

            Threading::newThread(
                [acc_cookie, place_id_str, job_id_str, account_id = account.id, account_display_name = account.
                    displayName] {
                    LOG_INFO(
                        "Generating launch link for account: " + account_display_name + " (ID: " + to_string(account_id)
                        + ") for place: " + place_id_str + (job_id_str.empty() ? "" : " job: " + job_id_str));
                    bool hasJob = !job_id_str.empty();
                    auto now_ms = chrono::duration_cast<chrono::milliseconds>(
                        chrono::system_clock::now().time_since_epoch()
                    ).count();

                    thread_local mt19937_64 rng{random_device{}()};
                    static uniform_int_distribution<int> d1(100000, 130000), d2(100000, 900000);

                    string browserTracker = to_string(d1(rng)) + to_string(d2(rng));
                    string ticket = RobloxApi::fetchAuthTicket(acc_cookie);
                    if (ticket.empty()) {
                        LOG_ERROR(
                            "Failed to grab auth ticket for account ID " + to_string(account_id) +
                            " while generating launch link.");
                        return;
                    }
                    Status::Set("Got auth ticket");
                    LOG_INFO("Successfully fetched auth ticket for account ID " + to_string(account_id));

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

                    Status::Set("Copied link to clipboard!");
                    SetClipboardText(uri.c_str());
                    LOG_INFO("Launch link copied to clipboard for account ID " + to_string(account_id));
                });
        }


        Separator();

        if (MenuItem("Copy UserID")) {
            SetClipboardText(account.userId.c_str());
            LOG_INFO("Copied UserID for account: " + account.displayName);
        }
        if (MenuItem("Copy Cookie")) {
            if (!account.cookie.empty()) {
                SetClipboardText(account.cookie.c_str());
                LOG_INFO("Copied cookie for account: " + account.displayName);
            } else {
                printf("Info: Cookie for account ID %d (%s) is empty.\n", account.id, account.displayName.c_str());
                LOG_WARN(
                    "Attempted to copy empty cookie for account: " + account.displayName + " (ID: " + to_string(account.
                        id) + ")");
                SetClipboardText("");
            }
        }
        if (MenuItem("Copy Display Name")) {
            SetClipboardText(account.displayName.c_str());
            LOG_INFO("Copied Display Name for account: " + account.displayName);
        }
        if (MenuItem("Copy Username")) {
            SetClipboardText(account.username.c_str());
            LOG_INFO("Copied Username for account: " + account.displayName);
        }
        if (MenuItem("Copy Note")) {
            SetClipboardText(account.note.c_str());
            LOG_INFO("Copied Note for account: " + account.displayName);
        }

        Separator();

        PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
        if (MenuItem("Delete This Account")) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Delete %s?", account.displayName.c_str());
            ConfirmPopup::Add(buf, [id = account.id, displayName = account.displayName]() {
                LOG_INFO("Attempting to delete account: " + displayName + " (ID: " + to_string(id) + ")");
                erase_if(
                    g_accounts,
                    [&](const AccountData &acc_data) {
                        return acc_data.id == id;
                    });
                g_selectedAccountIds.erase(id);
                Status::Set("Deleted account " + displayName);
                Data::SaveAccounts();
                LOG_INFO("Successfully deleted account: " + displayName + " (ID: " + to_string(id) + ")");
            });
        }
        PopStyleColor();

        if (!g_selectedAccountIds.empty() && g_selectedAccountIds.size() > 1 && g_selectedAccountIds.
            contains(account.id)) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Delete %zu Selected Account(s)", g_selectedAccountIds.size());
            PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
            if (MenuItem(buf)) {
                ConfirmPopup::Add("Delete selected accounts?", []() {
                    LOG_INFO("Attempting to delete " + to_string(g_selectedAccountIds.size()) + " selected accounts.");
                    erase_if(
                        g_accounts,
                        [&](const AccountData &acc_data) {
                            return g_selectedAccountIds.contains(acc_data.id);
                        });
                    g_selectedAccountIds.clear();
                    Data::SaveAccounts();
                    Status::Set("Deleted selected accounts");
                    LOG_INFO("Successfully deleted selected accounts.");
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
            LaunchWebview(g_customUrlBuffer, account.username + " - " + account.userId, account.cookie);
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
