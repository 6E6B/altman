#include <algorithm>
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <imgui.h>
#include <array>
#include <string>

#include "network/roblox.h"
#include "system/threading.h"
#include "system/roblox_control.h"
#include "ui/confirm.h"
#include "core/app_state.h"
#include "components.h"
#include "data.h"

using namespace ImGui;
using namespace std;

bool g_multiRobloxEnabled = false;
static HANDLE g_hMutex = nullptr;

static bool s_openClearCachePopup = false;
static void EnableMultiInstance() {
	if (!g_hMutex)
		g_hMutex = CreateMutexW(nullptr, FALSE, L"ROBLOX_singletonEvent");
}

static void DisableMultiInstance() {
	if (g_hMutex) {
		CloseHandle(g_hMutex);
		g_hMutex = nullptr;
	}
}

	static array<char, 2048> s_cookieInputBuffer = {};

	if (BeginMainMenuBar()) {
		if (BeginMenu("Accounts")) {
			if (MenuItem("Refresh Statuses")) {
				Threading::newThread([] {
					LOG_INFO("Refreshing account statuses...");
					for (auto &acct: g_accounts) {
						acct.status = Roblox::getPresence(acct.cookie, stoull(acct.userId));
						auto vs = Roblox::getVoiceChatStatus(acct.cookie);
						acct.voiceStatus = vs.status;
						acct.voiceBanExpiry = vs.bannedUntil;
					}
					Data::SaveAccounts();
					LOG_INFO("Refreshed account statuses");
				});
			}

			Separator();

			if (BeginMenu("Add Account")) {
				if (BeginMenu("Add via Cookie")) {
					TextUnformatted("Enter Cookie:");
					PushItemWidth(GetFontSize() * 25);
					InputText("##CookieInputSubmenu",
					          s_cookieInputBuffer.data(),
					          s_cookieInputBuffer.size(),
					          ImGuiInputTextFlags_AutoSelectAll);
					PopItemWidth();

					bool canAdd = (s_cookieInputBuffer[0] != '\0');
					if (canAdd && MenuItem("Add Cookie", nullptr, false, canAdd)) {
						const string cookie = s_cookieInputBuffer.data();
						try {
							int maxId = 0;
							for (auto &acct: g_accounts) {
								if (acct.id > maxId)
									maxId = acct.id;
							}
							int nextId = maxId + 1;

							uint64_t uid = Roblox::getUserId(cookie);
							string username = Roblox::getUsername(cookie);
							string displayName = Roblox::getDisplayName(cookie);
							string presence = Roblox::getPresence(cookie, uid);
							auto vs = Roblox::getVoiceChatStatus(cookie);

							AccountData newAcct;
							newAcct.id = nextId;
							newAcct.cookie = cookie;
							newAcct.userId = to_string(uid);
							newAcct.username = move(username);
							newAcct.displayName = move(displayName);
							newAcct.status = move(presence);
							newAcct.voiceStatus = vs.status;
							newAcct.voiceBanExpiry = vs.bannedUntil;
							newAcct.note = "";
							newAcct.isFavorite = false;

							g_accounts.push_back(move(newAcct));

							LOG_INFO("Added account " +
								to_string(nextId) + " - " +
								g_accounts.back().displayName.c_str());

							Data::SaveAccounts();
						} catch (const exception &ex) {
							LOG_ERROR(string("Could not add account via cookie: ") + ex.what());
						}
						s_cookieInputBuffer.fill('\0');
					}
					ImGui::EndMenu();
				}
				ImGui::EndMenu();
			}

			if (!g_selectedAccountIds.empty()) {
				Separator();
				char buf[64];
				snprintf(buf, sizeof(buf), "Delete %zu Selected", g_selectedAccountIds.size());
				PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
				if (MenuItem(buf)) {
					ConfirmPopup::Add("Delete selected accounts?", []() {
						erase_if(
							g_accounts,
							[&](const AccountData &acct) {
								return g_selectedAccountIds.count(acct.id);
							});
						g_selectedAccountIds.clear();
						Data::SaveAccounts();
						LOG_INFO("Deleted selected accounts.");
					});
				}
				PopStyleColor();
			}
			ImGui::EndMenu();
		}

		if (BeginMenu("Utilities")) {
                        if (MenuItem("Kill Roblox")) {
                                Threading::newThread(killRoblox);
                        }
                        if (MenuItem("Clear Roblox Cache")) {
                                if (isRobloxRunning()) {
                                        s_openClearCachePopup = true;
                                } else {
                                        Threading::newThread(clearRobloxCache);
                                }
                        }


			Separator();

			if (MenuItem("Multi Roblox  \xEF\x81\xB1", nullptr, &g_multiRobloxEnabled)) {
				if (g_multiRobloxEnabled) {
					EnableMultiInstance();
					LOG_INFO("Multi Roblox enabled.");
				} else {
					DisableMultiInstance();
					LOG_INFO("Multi Roblox disabled.");
				}
			}

			ImGui::EndMenu();
		}

		EndMainMenuBar();
        }
        if (s_openClearCachePopup) {
                ImGui::OpenPopup("ClearRobloxCacheConfirm");
                s_openClearCachePopup = false;
        }
        if (BeginPopupModal("ClearRobloxCacheConfirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                TextWrapped("RobloxPlayerBeta is running. Kill it before clearing the cache?");
                Spacing();
                if (Button("Kill", ImVec2(100, 0))) {
                        Threading::newThread([] { killRoblox(); clearRobloxCache(); });
                        CloseCurrentPopup();
                }
                SameLine();
                if (Button("Don't kill", ImVec2(100, 0))) {
                        Threading::newThread(clearRobloxCache);
                        CloseCurrentPopup();
                }
                SameLine();
                if (Button("Cancel", ImVec2(100, 0))) {
                        CloseCurrentPopup();
                }
                EndPopup();
        }

	if (BeginPopupModal("AddAccountPopup_Browser", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		Text("Browser-based account addition not yet implemented.");
		Separator();
		if (Button("OK", ImVec2(120, 0)))
			CloseCurrentPopup();
		EndPopup();
	}

	return false;
}
