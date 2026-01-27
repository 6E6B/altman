#include "roblox_control.h"

#include <format>
#include <string>

#include "console/console.h"

#ifdef _WIN32
#include <cstring>

#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>
#elif __APPLE__
#include <filesystem>
#include <fstream>
#include <vector>

#include <libproc.h>
#include <signal.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif

namespace RobloxControl {

#ifdef _WIN32
    namespace {
        std::string WStringToString(std::wstring_view wstr) {
            if (wstr.empty()) {
                return {};
            }
            int size_needed = WideCharToMultiByte(
                CP_UTF8,
                0,
                wstr.data(),
                static_cast<int>(wstr.size()),
                nullptr,
                0,
                nullptr,
                nullptr
            );
            std::string result(size_needed, '\0');
            WideCharToMultiByte(
                CP_UTF8,
                0,
                wstr.data(),
                static_cast<int>(wstr.size()),
                result.data(),
                size_needed,
                nullptr,
                nullptr
            );
            return result;
        }

        bool DeleteFileWithRetry(const std::wstring &path, int attempts = 50, int delayMs = 100) {
            for (int i = 0; i < attempts; ++i) {
                if (DeleteFileW(path.c_str())) {
                    return true;
                }
                DWORD err = GetLastError();
                if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED) {
                    LOG_ERROR("Failed to delete file: {} (Error: {})", WStringToString(path), err);
                    return false;
                }
                Sleep(delayMs);
            }
            LOG_ERROR("Timed out waiting to delete file: {}", WStringToString(path));
            return false;
        }

        bool RemoveDirectoryWithRetry(const std::wstring &path, int attempts = 50, int delayMs = 100) {
            for (int i = 0; i < attempts; ++i) {
                if (RemoveDirectoryW(path.c_str())) {
                    return true;
                }
                DWORD err = GetLastError();
                if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED) {
                    LOG_ERROR("Failed to remove directory: {} (Error: {})", WStringToString(path), err);
                    return false;
                }
                Sleep(delayMs);
            }
            LOG_ERROR("Timed out waiting to remove directory: {}", WStringToString(path));
            return false;
        }

        void ClearDirectoryContents(const std::wstring &directoryPath) {
            std::wstring searchPath = directoryPath + L"\\*";
            WIN32_FIND_DATAW findFileData;
            HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findFileData);

            if (hFind == INVALID_HANDLE_VALUE) {
                DWORD error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                    LOG_INFO(
                        "ClearDirectoryContents: Directory to clear not found or is empty: {}",
                        WStringToString(directoryPath)
                    );
                } else {
                    LOG_ERROR(
                        "ClearDirectoryContents: Failed to find first file in directory: {} (Error: {})",
                        WStringToString(directoryPath),
                        error
                    );
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
                        LOG_INFO("ClearDirectoryContents: Removed sub-directory: {}", WStringToString(itemFullPath));
                    } else {
                        LOG_ERROR(
                            "ClearDirectoryContents: Failed to remove sub-directory: {}",
                            WStringToString(itemFullPath)
                        );
                    }
                } else {
                    if (DeleteFileWithRetry(itemFullPath)) {
                        LOG_INFO("ClearDirectoryContents: Deleted file: {}", WStringToString(itemFullPath));
                    } else {
                        LOG_ERROR("ClearDirectoryContents: Failed to delete file: {}", WStringToString(itemFullPath));
                    }
                }
            } while (FindNextFileW(hFind, &findFileData) != 0);

            FindClose(hFind);

            DWORD lastError = GetLastError();
            if (lastError != ERROR_NO_MORE_FILES) {
                LOG_ERROR(
                    "ClearDirectoryContents: Error during file iteration in directory: {} (Error: {})",
                    WStringToString(directoryPath),
                    lastError
                );
            }
        }
    } // namespace

    bool IsRobloxRunning() {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) {
            return false;
        }
        PROCESSENTRY32 pe {};
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

    void KillRobloxProcesses() {
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32 pe {};
        pe.dwSize = sizeof(pe);
        if (hSnap == INVALID_HANDLE_VALUE) {
            return;
        }

        if (Process32First(hSnap, &pe)) {
            do {
                if (_stricmp(pe.szExeFile, "RobloxPlayerBeta.exe") == 0) {
                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (hProc) {
                        TerminateProcess(hProc, 0);
                        CloseHandle(hProc);
                        LOG_INFO("Terminated Roblox process: {}", pe.th32ProcessID);
                    } else {
                        LOG_ERROR(
                            "Failed to open Roblox process for termination: {} (Error: {})",
                            pe.th32ProcessID,
                            GetLastError()
                        );
                    }
                }
            } while (Process32Next(hSnap, &pe));
        } else {
            LOG_ERROR("Process32First failed when trying to kill Roblox. (Error: {})", GetLastError());
        }
        CloseHandle(hSnap);
        LOG_INFO("Kill Roblox process completed.");
    }

    void ClearRobloxCache() {
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
        LOG_INFO("Processing directory for full removal: {}", WStringToString(localStoragePath));
        if (directoryExists(localStoragePath)) {
            ClearDirectoryContents(localStoragePath);
            if (RemoveDirectoryWithRetry(localStoragePath)) {
                LOG_INFO("Successfully removed directory: {}", WStringToString(localStoragePath));
            } else {
                LOG_ERROR("Failed to remove directory: {}", WStringToString(localStoragePath));
            }
        } else {
            LOG_INFO("Directory not found, skipping: {}", WStringToString(localStoragePath));
        }

        std::wstring otaPatchBackupsPath = localAppDataPath_ws + L"\\Roblox\\OTAPatchBackups";
        LOG_INFO("Processing directory for full removal: {}", WStringToString(otaPatchBackupsPath));
        if (directoryExists(otaPatchBackupsPath)) {
            ClearDirectoryContents(otaPatchBackupsPath);
            if (RemoveDirectoryWithRetry(otaPatchBackupsPath)) {
                LOG_INFO("Successfully removed directory: {}", WStringToString(otaPatchBackupsPath));
            } else {
                LOG_ERROR("Failed to remove directory: {}", WStringToString(otaPatchBackupsPath));
            }
        } else {
            LOG_INFO("Directory not found, skipping: {}", WStringToString(otaPatchBackupsPath));
        }

        std::wstring robloxBasePath = localAppDataPath_ws + L"\\Roblox";
        std::wstring rbxStoragePattern = robloxBasePath + L"\\rbx-storage.*";
        LOG_INFO("Attempting to delete files matching pattern: {}", WStringToString(rbxStoragePattern));

        WIN32_FIND_DATAW findRbxData;
        HANDLE hFindRbx = FindFirstFileW(rbxStoragePattern.c_str(), &findRbxData);
        if (hFindRbx != INVALID_HANDLE_VALUE) {
            do {
                if (!(findRbxData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    && wcscmp(findRbxData.cFileName, L".") != 0 && wcscmp(findRbxData.cFileName, L"..") != 0) {
                    std::wstring filePathToDelete = robloxBasePath + L"\\" + findRbxData.cFileName;
                    if (DeleteFileWithRetry(filePathToDelete)) {
                        LOG_INFO("Deleted file: {}", WStringToString(filePathToDelete));
                    } else {
                        LOG_ERROR("Failed to delete file: {}", WStringToString(filePathToDelete));
                    }
                }
            } while (FindNextFileW(hFindRbx, &findRbxData) != 0);
            FindClose(hFindRbx);
        } else {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND) {
                LOG_INFO("No rbx-storage.* files found in: {}", WStringToString(robloxBasePath));
            } else {
                LOG_ERROR("Failed to search for rbx-storage.* files: {}", error);
            }
        }

        LOG_INFO("Roblox cache clearing process finished.");
    }

#elif __APPLE__
    bool IsRobloxRunning() {
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
        size_t size = 0;
        if (sysctl(mib, 4, nullptr, &size, nullptr, 0) < 0) {
            return false;
        }

        std::vector<kinfo_proc> procs(size / sizeof(kinfo_proc));
        if (sysctl(mib, 4, procs.data(), &size, nullptr, 0) < 0) {
            return false;
        }

        for (const auto &proc: procs) {
            if (std::strcmp(proc.kp_proc.p_comm, "RobloxPlayer") == 0) {
                return true;
            }
        }
        return false;
    }

    void KillRobloxProcesses() {
        LOG_INFO("Attempting to kill Roblox processes on macOS...");

        int result = system("pkill -9 'RobloxPlayer'");

        if (result == 0) {
            LOG_INFO("Successfully killed Roblox processes");
        } else {
            LOG_INFO("No Roblox processes found or failed to kill (this is normal if Roblox isn't running)");
        }
    }

    void ClearRobloxCache() {
        LOG_INFO("Starting Roblox cache clearing process on macOS...");

        const char *home = getenv("HOME");
        if (!home) {
            LOG_ERROR("Failed to get HOME directory");
            return;
        }

        std::vector<std::string> cachePaths
            = {std::string(home) + "/Library/Caches/com.roblox.RobloxPlayer",
               std::string(home) + "/Library/Roblox/LocalStorage",
               std::string(home) + "/Library/Roblox/OTAPatchBackups",
               std::string(home) + "/Library/Saved Application State/com.roblox.RobloxPlayer.savedState",
               std::string(home) + "/Library/HTTPStorages/com.Roblox.Roblox",
               std::string(home) + "/Library/HTTPStorages/com.roblox.RobloxPlayer",
               std::string(home) + "/Library/WebKit/com.roblox.RobloxPlayer"};

        for (const auto &path: cachePaths) {
            try {
                if (std::filesystem::exists(path)) {
                    LOG_INFO("Clearing cache directory: {}", path);

                    std::uintmax_t removed = std::filesystem::remove_all(path);

                    if (removed > 0) {
                        LOG_INFO("Removed {} items from: {}", removed, path);
                    } else {
                        LOG_INFO("Directory was empty or already removed: {}", path);
                    }
                } else {
                    LOG_INFO("Cache directory not found (this is normal): {}", path);
                }
            } catch (const std::filesystem::filesystem_error &e) {
                LOG_ERROR("Failed to clear cache at {}: {}", path, e.what());
            }
        }

        std::filesystem::path baseDir = std::string(home) + "/Library/Roblox/";
        if (!std::filesystem::exists(baseDir) || !std::filesystem::is_directory(baseDir)) {
            LOG_INFO("Roblox base directory not found. Skipping rbx-storage cleanup.");
        } else {
            for (const auto &entry: std::filesystem::directory_iterator(baseDir)) {
                if (!entry.is_regular_file()) {
                    continue;
                }

                std::string filename = entry.path().filename().string();
                if (filename.rfind("rbx-storage", 0) == 0) {
                    try {
                        std::filesystem::remove(entry.path());
                        LOG_INFO("Deleted: {}", entry.path().string());
                    } catch (const std::exception &e) {
                        LOG_ERROR("Failed to delete: {} ({})", entry.path().string(), e.what());
                    }
                }
            }
        }

        LOG_INFO("Roblox cache clearing process finished.");
    }

#else

    bool IsRobloxRunning() {
        LOG_WARN("IsRobloxRunning not implemented for this platform");
        return false;
    }

    void KillRobloxProcesses() {
        LOG_WARN("KillRobloxProcesses not implemented for this platform");
    }

    void ClearRobloxCache() {
        LOG_WARN("ClearRobloxCache not implemented for this platform");
    }

#endif

} // namespace RobloxControl
