#pragma once
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <string>
#include <cstring>

#include "../core/logging.hpp"

namespace RobloxControl {
inline HANDLE g_multiMutex = nullptr;

inline void EnableMultiInstance() {
    if (!g_multiMutex)
        g_multiMutex = CreateMutexW(nullptr, FALSE, L"ROBLOX_singletonEvent");
}

inline void DisableMultiInstance() {
    if (g_multiMutex) {
        CloseHandle(g_multiMutex);
        g_multiMutex = nullptr;
    }
}
inline bool IsRobloxRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    bool running = false;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, "RobloxPlayerBeta.exe") == 0) {
                running = true;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return running;
}

inline void KillRobloxProcesses() {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    if (hSnap == INVALID_HANDLE_VALUE)
        return;

    if (Process32First(hSnap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, "RobloxPlayerBeta.exe") == 0) {
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hProc) {
                    TerminateProcess(hProc, 0);
                    CloseHandle(hProc);
                    LOG_INFO(std::string("Terminated Roblox process: ") + std::to_string(pe.th32ProcessID));
                } else {
                    LOG_ERROR(std::string("Failed to open Roblox process for termination: ") +
                              std::to_string(pe.th32ProcessID) + " (Error: " + std::to_string(GetLastError()) + ")");
                }
            }
        } while (Process32Next(hSnap, &pe));
    } else {
        LOG_ERROR(std::string("Process32First failed when trying to kill Roblox. (Error: ") +
                  std::to_string(GetLastError()) + ")");
    }
    CloseHandle(hSnap);
    LOG_INFO("Kill Roblox process completed.");
}

inline std::string WStringToString(const std::wstring &wstr) {
    if (wstr.empty())
        return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], static_cast<int>(wstr.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], static_cast<int>(wstr.size()), &strTo[0],
                        size_needed, nullptr, nullptr);
    return strTo;
}

inline bool DeleteFileWithRetry(const std::wstring &path,
                                int attempts = 50,
                                int delayMs = 100) {
    for (int i = 0; i < attempts; ++i) {
        if (DeleteFileW(path.c_str()))
            return true;
        DWORD err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED) {
            LOG_ERROR("Failed to delete file: " + WStringToString(path) +
                      " (Error: " + std::to_string(err) + ")");
            return false;
        }
        Sleep(delayMs);
    }
    LOG_ERROR("Timed out waiting to delete file: " + WStringToString(path));
    return false;
}

inline bool RemoveDirectoryWithRetry(const std::wstring &path,
                                     int attempts = 50,
                                     int delayMs = 100) {
    for (int i = 0; i < attempts; ++i) {
        if (RemoveDirectoryW(path.c_str()))
            return true;
        DWORD err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED) {
            LOG_ERROR("Failed to remove directory: " + WStringToString(path) +
                      " (Error: " + std::to_string(err) + ")");
            return false;
        }
        Sleep(delayMs);
    }
    LOG_ERROR("Timed out waiting to remove directory: " + WStringToString(path));
    return false;
}

inline void ClearDirectoryContents(const std::wstring &directoryPath) {
    std::wstring searchPath = directoryPath + L"\\*";
    WIN32_FIND_DATAW findFileData;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findFileData);

    if (hFind == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();

        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            LOG_INFO("ClearDirectoryContents: Directory to clear not found or is empty: " +
                     WStringToString(directoryPath));
        } else {
            LOG_ERROR("ClearDirectoryContents: Failed to find first file in directory: " +
                      WStringToString(directoryPath) + " (Error: " + std::to_string(error) + ")");
        }
        return;
    }

    do {
        const std::wstring itemName = findFileData.cFileName;
        if (itemName == L"." || itemName == L"..") {
            continue;
        }

        std::wstring itemFullPath = directoryPath + L"\\" + itemName;

        if (findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ClearDirectoryContents(itemFullPath);

            if (RemoveDirectoryWithRetry(itemFullPath)) {
                LOG_INFO("ClearDirectoryContents: Removed sub-directory: " + WStringToString(itemFullPath));
            } else {
                LOG_ERROR("ClearDirectoryContents: Failed to remove sub-directory: " +
                          WStringToString(itemFullPath));
            }
        } else {
            if (DeleteFileWithRetry(itemFullPath)) {
                LOG_INFO("ClearDirectoryContents: Deleted file: " + WStringToString(itemFullPath));
            } else {
                LOG_ERROR("ClearDirectoryContents: Failed to delete file: " +
                          WStringToString(itemFullPath));
            }
        }
    } while (FindNextFileW(hFind, &findFileData) != 0);

    FindClose(hFind);

    DWORD lastError = GetLastError();
    if (lastError != ERROR_NO_MORE_FILES) {
        LOG_ERROR("ClearDirectoryContents: Error during file iteration in directory: " +
                  WStringToString(directoryPath) + " (Error: " + std::to_string(lastError) + ")");
    }
}

inline void ClearRobloxCache() {
    LOG_INFO("Starting extended Roblox cache clearing process...");

    WCHAR localAppDataPath_c[MAX_PATH];
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, localAppDataPath_c))) {
        LOG_ERROR("Failed to get Local AppData path. Aborting cache clear.");
        return;
    }
    std::wstring localAppDataPath_ws = localAppDataPath_c;

    auto directoryExists = [](const std::wstring &path) -> bool {
        DWORD attrib = GetFileAttributesW(path.c_str());
        return (attrib != INVALID_FILE_ATTRIBUTES && (attrib & FILE_ATTRIBUTE_DIRECTORY));
    };

    std::wstring localStoragePath = localAppDataPath_ws + L"\\Roblox\\LocalStorage";
    LOG_INFO("Processing directory for full removal: " + WStringToString(localStoragePath));
    if (directoryExists(localStoragePath)) {
        ClearDirectoryContents(localStoragePath);
        if (RemoveDirectoryWithRetry(localStoragePath)) {
            LOG_INFO("Successfully removed directory: " + WStringToString(localStoragePath));
        } else {
            LOG_ERROR("Failed to remove directory (it might not be fully empty or is in use): " +
                      WStringToString(localStoragePath));
        }
    } else {
        LOG_INFO("Directory not found, skipping: " + WStringToString(localStoragePath));
    }

    std::wstring otaPatchBackupsPath = localAppDataPath_ws + L"\\Roblox\\OTAPatchBackups";
    LOG_INFO("Processing directory for full removal: " + WStringToString(otaPatchBackupsPath));
    if (directoryExists(otaPatchBackupsPath)) {
        ClearDirectoryContents(otaPatchBackupsPath);
        if (RemoveDirectoryWithRetry(otaPatchBackupsPath)) {
            LOG_INFO("Successfully removed directory: " + WStringToString(otaPatchBackupsPath));
        } else {
            LOG_ERROR("Failed to remove directory (it might not be fully empty or is in use): " +
                      WStringToString(otaPatchBackupsPath));
        }
    } else {
        LOG_INFO("Directory not found, skipping: " + WStringToString(otaPatchBackupsPath));
    }

    std::wstring robloxBasePath = localAppDataPath_ws + L"\\Roblox";
    std::wstring rbxStoragePattern = robloxBasePath + L"\\rbx-storage.*";
    LOG_INFO("Attempting to delete files matching pattern: " + WStringToString(rbxStoragePattern));

    WIN32_FIND_DATAW findRbxData;
    HANDLE hFindRbx = FindFirstFileW(rbxStoragePattern.c_str(), &findRbxData);
    if (hFindRbx != INVALID_HANDLE_VALUE) {
        do {
            if (!(findRbxData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                wcscmp(findRbxData.cFileName, L".") != 0 &&
                wcscmp(findRbxData.cFileName, L"..") != 0) {
                std::wstring filePathToDelete = robloxBasePath + L"\\" + findRbxData.cFileName;
                if (DeleteFileWithRetry(filePathToDelete)) {
                    LOG_INFO("Deleted file: " + WStringToString(filePathToDelete));
                } else {
                    LOG_ERROR("Failed to delete file: " + WStringToString(filePathToDelete));
                }
            }
        } while (FindNextFileW(hFindRbx, &findRbxData) != 0);
        FindClose(hFindRbx);
        DWORD findLastError = GetLastError();
        if (findLastError != ERROR_NO_MORE_FILES) {
            LOG_ERROR("Error during rbx-storage.* file iteration: " + WStringToString(robloxBasePath) +
                      " (Error: " + std::to_string(findLastError) + ")");
        }
    } else {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            LOG_INFO("No rbx-storage.* files found in: " + WStringToString(robloxBasePath));
        } else {
            LOG_ERROR("Failed to search for rbx-storage.* files in: " + WStringToString(robloxBasePath) +
                      " (Error: " + std::to_string(error) + ")");
        }
    }

    LOG_INFO("Roblox cache clearing process finished.");
}

} // namespace RobloxControl
#endif
