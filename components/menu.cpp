#include <algorithm>
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <imgui.h>
#include <array>
#include <string>

#include "../utils/network/roblox_api.h"
#include "../utils/threads/threading.h"
#include "../utils/gui/confirm.h"
#include "../utils/misc/app_state.h"
#include "components.h"
#include "data.h"

using namespace ImGui;
using namespace std;

bool g_multiRobloxEnabled = false;
static HANDLE g_hMutex = nullptr;

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

static string WStringToString(const wstring &wstr) {
    if (wstr.empty())
        return string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], static_cast<int>(wstr.size()), nullptr, 0, nullptr,
                                          nullptr);
    string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], static_cast<int>(wstr.size()), &strTo[0], size_needed, nullptr, nullptr);
    return strTo;
}

static void ClearDirectoryContents(const wstring &directoryPath) {
    wstring searchPath = directoryPath + L"\\*";
    WIN32_FIND_DATAW findFileData;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findFileData);

    if (hFind == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();

        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            LOG_INFO(
                "ClearDirectoryContents: Directory to clear not found or is empty: " + WStringToString(directoryPath));
        } else {
            LOG_ERROR(
                "ClearDirectoryContents: Failed to find first file in directory: " + WStringToString(directoryPath) +
                " (Error: " + to_string(error) + ")");
        }
        return;
    }

    do {
        const wstring itemName = findFileData.cFileName;
        if (itemName == L"." || itemName == L"..") {
            continue;
        }

        wstring itemFullPath = directoryPath + L"\\" + itemName;

        if (findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ClearDirectoryContents(itemFullPath);

            if (!RemoveDirectoryW(itemFullPath.c_str())) {
                LOG_ERROR(
                    "ClearDirectoryContents: Failed to remove sub-directory: " + WStringToString(itemFullPath) +
                    " (Error: " + to_string(GetLastError()) + ")");
            } else {
                LOG_INFO("ClearDirectoryContents: Removed sub-directory: " + WStringToString(itemFullPath));
            }
        } else {
            if (!DeleteFileW(itemFullPath.c_str())) {
                LOG_ERROR(
                    "ClearDirectoryContents: Failed to delete file: " + WStringToString(itemFullPath) + " (Error: " +
                    to_string(GetLastError()) + ")");
            } else {
                LOG_INFO("ClearDirectoryContents: Deleted file: " + WStringToString(itemFullPath));
            }
        }
    } while (FindNextFileW(hFind, &findFileData) != 0);

    FindClose(hFind);

    DWORD lastError = GetLastError();
    if (lastError != ERROR_NO_MORE_FILES) {
        LOG_ERROR(
            "ClearDirectoryContents: Error during file iteration in directory: " + WStringToString(directoryPath) +
            " (Error: " + to_string(lastError) + ")");
    }
}

bool RenderMainMenu() {
    static array<char, 2048> s_cookieInputBuffer = {};

    if (BeginMainMenuBar()) {
        if (BeginMenu("Accounts")) {
            if (MenuItem("Refresh Statuses")) {
                Threading::newThread([] {
                    LOG_INFO("Refreshing account statuses...");
                    for (auto &acct: g_accounts) {
                        acct.status = RobloxApi::getPresence(acct.cookie, stoull(acct.userId));
                        auto vs = RobloxApi::getVoiceChatStatus(acct.cookie);
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

                            uint64_t uid = RobloxApi::getUserId(cookie);
                            string username = RobloxApi::getUsername(cookie);
                            string displayName = RobloxApi::getDisplayName(cookie);
                            string presence = RobloxApi::getPresence(cookie, uid);
                            auto vs = RobloxApi::getVoiceChatStatus(cookie);

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
                Threading::newThread([] {
                    LOG_INFO("Starting extended Roblox cache clearing process...");

                    WCHAR localAppDataPath_c[MAX_PATH];
                    if (!SUCCEEDED(
                        SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, localAppDataPath_c))) {
                        LOG_ERROR("Failed to get Local AppData path. Aborting cache clear.");
                        return;
                    }
                    wstring localAppDataPath_ws = localAppDataPath_c;

                    auto directoryExists = [](const wstring &path) -> bool {
                        DWORD attrib = GetFileAttributesW(path.c_str());
                        return (attrib != INVALID_FILE_ATTRIBUTES && (attrib & FILE_ATTRIBUTE_DIRECTORY));
                    };

                    wstring localStoragePath = localAppDataPath_ws + L"\\Roblox\\LocalStorage";
                    LOG_INFO("Processing directory for full removal: " + WStringToString(localStoragePath));
                    if (directoryExists(localStoragePath)) {
                        ClearDirectoryContents(localStoragePath);
                        if (RemoveDirectoryW(localStoragePath.c_str())) {
                            LOG_INFO("Successfully removed directory: " + WStringToString(localStoragePath));
                        } else {
                            LOG_ERROR(
                                "Failed to remove directory (it might not be fully empty or is in use): " +
                                WStringToString(localStoragePath) + " (Error: " + to_string(GetLastError()) + ")");
                        }
                    } else {
                        LOG_INFO("Directory not found, skipping: " + WStringToString(localStoragePath));
                    }

                    wstring otaPatchBackupsPath = localAppDataPath_ws + L"\\Roblox\\OTAPatchBackups";
                    LOG_INFO("Processing directory for full removal: " + WStringToString(otaPatchBackupsPath));
                    if (directoryExists(otaPatchBackupsPath)) {
                        ClearDirectoryContents(otaPatchBackupsPath);
                        if (RemoveDirectoryW(otaPatchBackupsPath.c_str())) {
                            LOG_INFO("Successfully removed directory: " + WStringToString(otaPatchBackupsPath));
                        } else {
                            LOG_ERROR(
                                "Failed to remove directory (it might not be fully empty or is in use): " +
                                WStringToString(otaPatchBackupsPath) + " (Error: " + to_string(GetLastError()) + ")");
                        }
                    } else {
                        LOG_INFO("Directory not found, skipping: " + WStringToString(otaPatchBackupsPath));
                    }

                    wstring robloxBasePath = localAppDataPath_ws + L"\\Roblox";
                    wstring rbxStoragePattern = robloxBasePath + L"\\rbx-storage.*";
                    LOG_INFO("Attempting to delete files matching pattern: " + WStringToString(rbxStoragePattern));

                    WIN32_FIND_DATAW findRbxData;
                    HANDLE hFindRbx = FindFirstFileW(rbxStoragePattern.c_str(), &findRbxData);
                    if (hFindRbx != INVALID_HANDLE_VALUE) {
                        do {
                            if (!(findRbxData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                                wcscmp(findRbxData.cFileName, L".") != 0 &&
                                wcscmp(findRbxData.cFileName, L"..") != 0) {
                                wstring filePathToDelete = robloxBasePath + L"\\" + findRbxData.cFileName;
                                if (DeleteFileW(filePathToDelete.c_str())) {
                                    LOG_INFO("Deleted file: " + WStringToString(filePathToDelete));
                                } else {
                                    LOG_ERROR(
                                        "Failed to delete file: " + WStringToString(filePathToDelete) + " (Error: " +
                                        to_string(GetLastError()) + ")");
                                }
                            }
                        } while (FindNextFileW(hFindRbx, &findRbxData) != 0);
                        FindClose(hFindRbx);
                        DWORD findLastError = GetLastError();
                        if (findLastError != ERROR_NO_MORE_FILES) {
                            LOG_ERROR(
                                "Error during rbx-storage.* file iteration: " + WStringToString(robloxBasePath) +
                                " (Error: " + to_string(findLastError) + ")");
                        }
                    } else {
                        DWORD error = GetLastError();
                        if (error == ERROR_FILE_NOT_FOUND) {
                            LOG_INFO("No rbx-storage.* files found in: " + WStringToString(robloxBasePath));
                        } else {
                            LOG_ERROR(
                                "Failed to search for rbx-storage.* files in: " + WStringToString(robloxBasePath) +
                                " (Error: " + to_string(error) + ")");
                        }
                    }

                    LOG_INFO("Roblox cache clearing process finished.");
                });
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

    if (BeginPopupModal("AddAccountPopup_Browser", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        Text("Browser-based account addition not yet implemented.");
        Separator();
        if (Button("OK", ImVec2(120, 0)))
            CloseCurrentPopup();
        EndPopup();
    }

    return false;
}
