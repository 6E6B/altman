#include <algorithm>
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <imgui.h>
#include <array>
#include <string>
#include <vector>
#include <filesystem>

#include "network/roblox.h"
#include "system/threading.h"
#include "system/roblox_control.h"
#include "system/multi_instance.h"
#include "ui/confirm.h"
#include "core/app_state.h"
#include "components.h"
#include "data.h"
#include "backup.h"
#include "ui/modal_popup.h"

using namespace ImGui;
using namespace std;

bool g_multiRobloxEnabled = false;

// Global state for duplicate account modal
static struct {
	bool showModal = false;
	string pendingCookie;
	string pendingUsername;
	string pendingDisplayName;
	string pendingPresence;
	string pendingUserId;
	Roblox::VoiceSettings pendingVoiceStatus;
	int existingId = -1;
	int nextId = -1;
} g_duplicateAccountModal;


bool RenderMainMenu() {
        static array<char, 2048> s_cookieInputBuffer = {};
        static bool s_openClearCachePopup = false;
        static bool s_openExportPopup = false;
        static bool s_openImportPopup = false;
        static char s_password1[128] = "";
        static char s_password2[128] = "";
        static char s_importPassword[128] = "";
        static std::vector<std::string> s_backupFiles;
        static int s_selectedBackup = 0;
        static bool s_refreshBackupList = false;

        if (BeginMainMenuBar()) {
                if (BeginMenu("File")) {
                        if (MenuItem("Export Backup")) {
                                s_openExportPopup = true;
                        }

                        if (MenuItem("Import Backup")) {
                                s_openImportPopup = true;
                        }
                        ImGui::EndMenu();
                }

                if (BeginMenu("Accounts")) {
                        if (MenuItem("Refresh Statuses")) {
                                Threading::newThread([] {
                                        LOG_INFO("Refreshing account statuses...");
                                        for (auto &acct: g_accounts) {
                                                auto banStatus = Roblox::refreshBanStatus(acct.cookie);
                                                if (banStatus == Roblox::BanCheckResult::Banned) {
                                                        acct.status = "Banned";
                                                        acct.voiceStatus = "N/A";
                                                        acct.voiceBanExpiry = 0;
                                                        continue;
                                                }
                                                if (banStatus == Roblox::BanCheckResult::Warned) {
                                                        acct.status = "Warned";
                                                        acct.voiceStatus = "N/A";
                                                        acct.voiceBanExpiry = 0;
                                                        continue;  // Skip processing like banned accounts
                                                }
                                                if (banStatus == Roblox::BanCheckResult::Terminated) {
                                                        acct.status = "Terminated";
                                                        acct.voiceStatus = "N/A";
                                                        acct.voiceBanExpiry = 0;
                                                        continue;
                                                }

						if (!acct.userId.empty()) {
							acct.status = Roblox::getPresence(acct.cookie, stoull(acct.userId));
							auto vs = Roblox::getVoiceChatStatus(acct.cookie);
							acct.voiceStatus = vs.status;
							acct.voiceBanExpiry = vs.bannedUntil;
						}
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

							// Check if an account with this userId already exists
							auto existingAccount = find_if(g_accounts.begin(), g_accounts.end(),
								[&](const AccountData &a) { return a.userId == to_string(uid); });

							if (existingAccount != g_accounts.end()) {
								// Store data for modal
								g_duplicateAccountModal.pendingCookie = cookie;
								g_duplicateAccountModal.pendingUsername = username;
								g_duplicateAccountModal.pendingDisplayName = displayName;
								g_duplicateAccountModal.pendingPresence = presence;
								g_duplicateAccountModal.pendingUserId = to_string(uid);
								g_duplicateAccountModal.pendingVoiceStatus = vs;
								g_duplicateAccountModal.existingId = existingAccount->id;
								g_duplicateAccountModal.nextId = nextId;
								g_duplicateAccountModal.showModal = true;
							} else {
								// Create new account (no duplicate)
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

								LOG_INFO("Added new account " +
									to_string(nextId) + " - " +
									g_accounts.back().displayName.c_str());
								Data::SaveAccounts();
							}
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
				HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
				PROCESSENTRY32 pe;
				pe.dwSize = sizeof(pe);

				if (Process32First(hSnap, &pe)) {
					do {
						if (_stricmp(pe.szExeFile, "RobloxPlayerBeta.exe") == 0) {
							HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
							if (hProc) {
								TerminateProcess(hProc, 0);
								CloseHandle(hProc);
								LOG_INFO(string("Terminated Roblox process: ") + to_string(pe.th32ProcessID));
							} else {
								LOG_ERROR(
									string("Failed to open Roblox process for termination: ") + to_string(pe.
										th32ProcessID) + " (Error: " + to_string(GetLastError()) + ")");
							}
						}
					} while (Process32Next(hSnap, &pe));
				} else {
					LOG_ERROR(
						string("Process32First failed when trying to kill Roblox. (Error: ") + to_string(GetLastError())
						+ ")");
				}
				CloseHandle(hSnap);
				LOG_INFO("Kill Roblox process completed.");
			}

			if (MenuItem("Clear Roblox Cache")) {
				if (RobloxControl::IsRobloxRunning())
					s_openClearCachePopup = true;
				else
					Threading::newThread(RobloxControl::ClearRobloxCache);
			}

                        ImGui::EndMenu();
		}

		EndMainMenuBar();
	}

	if (s_openClearCachePopup) {
		OpenPopup("ClearCacheConfirm");
		s_openClearCachePopup = false;
	}

	if (BeginPopupModal("ClearCacheConfirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		TextWrapped("RobloxPlayerBeta is running. Do you want to kill it before clearing the cache?");
		Spacing();
		float killW = CalcTextSize("Kill").x + GetStyle().FramePadding.x * 2.0f;
		float dontW = CalcTextSize("Don't kill").x + GetStyle().FramePadding.x * 2.0f;
		float cancelW = CalcTextSize("Cancel").x + GetStyle().FramePadding.x * 2.0f;
		if (Button("Kill", ImVec2(killW, 0))) {
			RobloxControl::KillRobloxProcesses();
			Threading::newThread(RobloxControl::ClearRobloxCache);
			CloseCurrentPopup();
		}
		SameLine(0, GetStyle().ItemSpacing.x);
		if (Button("Don't kill", ImVec2(dontW, 0))) {
			Threading::newThread(RobloxControl::ClearRobloxCache);
			CloseCurrentPopup();
		}
		SameLine(0, GetStyle().ItemSpacing.x);
		if (Button("Cancel", ImVec2(cancelW, 0))) {
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

        if (s_openExportPopup) {
                OpenPopup("ExportBackup");
                s_openExportPopup = false;
        }

        if (BeginPopupModal("ExportBackup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                InputText("Password", s_password1, IM_ARRAYSIZE(s_password1), ImGuiInputTextFlags_Password);
                InputText("Confirm", s_password2, IM_ARRAYSIZE(s_password2), ImGuiInputTextFlags_Password);
                if (Button("Export")) {
                        if (strcmp(s_password1, s_password2) == 0 && s_password1[0] != '\0') {
                                if (Backup::Export(s_password1))
                                        ModalPopup::Add("Backup saved.");
                                else
                                        ModalPopup::Add("Backup failed.");
                                s_password1[0] = s_password2[0] = '\0';
                                CloseCurrentPopup();
                        } else {
                                ModalPopup::Add("Passwords do not match.");
                        }
                }
                SameLine();
                if (Button("Cancel")) {
                        CloseCurrentPopup();
                }
                EndPopup();
        }

        if (s_openImportPopup) {
                OpenPopup("ImportBackup");
                s_openImportPopup = false;
                s_refreshBackupList = true;
        }

        if (BeginPopupModal("ImportBackup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                if (s_refreshBackupList) {
                        s_backupFiles.clear();
                        std::filesystem::path dir = Data::StorageFilePath("backups");
                        std::error_code ec;
                        std::filesystem::create_directories(dir, ec);
                        for (const auto &entry : std::filesystem::directory_iterator(dir)) {
                                if (entry.is_regular_file())
                                        s_backupFiles.push_back(entry.path().filename().string());
                        }
                        std::sort(s_backupFiles.begin(), s_backupFiles.end());
                        s_selectedBackup = 0;
                        s_refreshBackupList = false;
                }

                if (s_backupFiles.empty()) {
                        TextUnformatted("No backups found.");
                } else {
                        const char *current = s_backupFiles[s_selectedBackup].c_str();
                        if (BeginCombo("File", current)) {
                                for (int i = 0; i < (int)s_backupFiles.size(); ++i) {
                                        bool selected = (i == s_selectedBackup);
                                        // Push a unique ID for each backup file item in the dropdown
                                        PushID(i);
                                        if (Selectable(s_backupFiles[i].c_str(), selected))
                                                s_selectedBackup = i;
                                        PopID();
                                        if (selected)
                                                SetItemDefaultFocus();
                                }
                                EndCombo();
                        }
                }
                InputText("Password", s_importPassword, IM_ARRAYSIZE(s_importPassword), ImGuiInputTextFlags_Password);
                if (Button("Import")) {
                        std::string err;
                        bool ok = false;
                        if (!s_backupFiles.empty()) {
                                std::string path = Data::StorageFilePath("backups/" + s_backupFiles[s_selectedBackup]);
                                ok = Backup::Import(path, s_importPassword, &err);
                        }
                        if (ok)
                                ModalPopup::Add("Import completed.");
                        else
                                ModalPopup::Add(err.empty() ? "Import failed." : err.c_str());
                        s_importPassword[0] = '\0';
                        CloseCurrentPopup();
                }
                SameLine();
                if (Button("Cancel")) {
                        CloseCurrentPopup();
                }
                EndPopup();
        }

	// Handle duplicate account prompt
	if (g_duplicateAccountModal.showModal) {
		OpenPopup("DuplicateAccountPrompt");
		g_duplicateAccountModal.showModal = false;
	}

	if (BeginPopupModal("DuplicateAccountPrompt", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		// Find existing account for display name
		auto existingAccount = find_if(g_accounts.begin(), g_accounts.end(),
			[](const AccountData &a) { 
				return a.id == g_duplicateAccountModal.existingId; 
			});
		
		if (existingAccount != g_accounts.end()) {
			char buf[256];
			snprintf(buf, sizeof(buf), 
				"The cookie you entered is for an already existing account (%s). What would you like to do?",
				existingAccount->displayName.c_str());
			TextWrapped(buf);
		} else {
			TextWrapped("The cookie you entered is for an already existing account. What would you like to do?");
		}
		
		Spacing();

		if (Button("Update", ImVec2(100, 0))) {
			// Update existing account
			auto it = find_if(g_accounts.begin(), g_accounts.end(),
				[](const AccountData &a) { return a.id == g_duplicateAccountModal.existingId; });
			if (it != g_accounts.end()) {
				it->cookie = g_duplicateAccountModal.pendingCookie;
				it->username = g_duplicateAccountModal.pendingUsername;
				it->displayName = g_duplicateAccountModal.pendingDisplayName;
				it->status = g_duplicateAccountModal.pendingPresence;
				it->voiceStatus = g_duplicateAccountModal.pendingVoiceStatus.status;
				it->voiceBanExpiry = g_duplicateAccountModal.pendingVoiceStatus.bannedUntil;

				LOG_INFO("Updated existing account " + to_string(it->id) + " - " + it->displayName);
				Data::SaveAccounts();
			}
			CloseCurrentPopup();
		}

		SameLine();
		if (Button("Discard", ImVec2(100, 0))) {
			LOG_INFO("Discarded new cookie for existing account " + to_string(g_duplicateAccountModal.existingId));
			CloseCurrentPopup();
		}

		SameLine();
		if (Button("Force Add", ImVec2(100, 0))) {
			// Create new account
			AccountData newAcct;
			newAcct.id = g_duplicateAccountModal.nextId;
			newAcct.cookie = g_duplicateAccountModal.pendingCookie;
			newAcct.userId = g_duplicateAccountModal.pendingUserId;
			newAcct.username = g_duplicateAccountModal.pendingUsername;
			newAcct.displayName = g_duplicateAccountModal.pendingDisplayName;
			newAcct.status = g_duplicateAccountModal.pendingPresence;
			newAcct.voiceStatus = g_duplicateAccountModal.pendingVoiceStatus.status;
			newAcct.voiceBanExpiry = g_duplicateAccountModal.pendingVoiceStatus.bannedUntil;
			newAcct.note = "";
			newAcct.isFavorite = false;

			g_accounts.push_back(move(newAcct));

			LOG_INFO("Force added new account " + to_string(g_duplicateAccountModal.nextId) + " - " + g_duplicateAccountModal.pendingDisplayName);
			Data::SaveAccounts();
			CloseCurrentPopup();
		}
		
		EndPopup();
	}

	return false;
}
