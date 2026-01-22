#include "auto_updater.h"

#include <format>
#include <print>
#include <fstream>
#include <algorithm>
#include <thread>
#include <array>
#include <ranges>

#include <imgui.h>

#include "version.h"
#include "console/console.h"
#include "utils/thread_task.h"
#include "utils/paths.h"
#include "network/http.h"
#include "ui/widgets/modal_popup.h"
#include "ui/widgets/notifications.h"

#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
    #ifdef __APPLE__
        #include <mach-o/dyld.h>
        #include <Security/Security.h>
        #include <CoreFoundation/CoreFoundation.h>
        #include <spawn.h>
        extern char **environ;
    #endif
#endif

std::filesystem::path UpdaterConfig::GetConfigPath() {
    return AltMan::Paths::Config("updater.json");
}

void UpdaterConfig::Save() const {
    const nlohmann::json j = {
        {"channel", static_cast<int>(channel)},
        {"autoCheck", autoCheck},
        {"autoDownload", autoDownload},
        {"autoInstall", autoInstall},
        {"showNotifications", showNotifications},
        {"bandwidthLimit", bandwidthLimit},
        {"lastCheck", std::chrono::system_clock::to_time_t(lastCheck)},
        {"lastInstalledVersion", lastInstalledVersion},
        {"backupPath", backupPath.string()},
        {"resumeFilePath", resumeFilePath.string()},
        {"resumeOffset", resumeOffset}
    };

    if (std::ofstream file(GetConfigPath()); file.is_open()) {
        file << j.dump(2);
    }
}

void UpdaterConfig::Load() {
    std::ifstream file(GetConfigPath());
    if (!file.is_open()) {
        return;
    }

    nlohmann::json j;
    file >> j;

    channel = static_cast<UpdateChannel>(j.value("channel", 0));
    autoCheck = j.value("autoCheck", true);
    autoDownload = j.value("autoDownload", false);
    autoInstall = j.value("autoInstall", false);
    showNotifications = j.value("showNotifications", true);
    bandwidthLimit = j.value("bandwidthLimit", 0);

    const auto lastCheckTime = j.value("lastCheck", 0LL);
    lastCheck = std::chrono::system_clock::from_time_t(lastCheckTime);

    lastInstalledVersion = j.value("lastInstalledVersion", "");
    backupPath = j.value("backupPath", "");
    resumeFilePath = j.value("resumeFilePath", "");
    resumeOffset = j.value("resumeOffset", 0);
}

namespace {
    UpdaterConfig config;
    std::filesystem::path pendingUpdatePath;
    DownloadState currentDownload;
}

constexpr std::string_view AutoUpdater::GetChannelName(UpdateChannel channel) noexcept {
    switch (channel) {
        case UpdateChannel::Beta: return "beta";
        case UpdateChannel::Dev: return "dev";
        default: return "stable";
    }
}

std::string AutoUpdater::GetPlatformAssetName(UpdateChannel channel) {
    std::string_view base;
    std::string_view extension;

#ifdef _WIN32
    base = "AltMan-Windows";
    extension = ".exe";
#elif __APPLE__
    base = "AltMan-macOS";
    extension = ".dmg";
#else
    base = "AltMan-Linux";
    extension = ".AppImage";
#endif

    if (channel == UpdateChannel::Stable) {
        return std::format("{}{}", base, extension);
    }
    return std::format("{}-{}{}", base, GetChannelName(channel), extension);
}

std::string AutoUpdater::GetDeltaAssetName(std::string_view fromVersion, std::string_view toVersion) {
#ifdef _WIN32
    constexpr std::string_view extension = ".patch";
#else
    constexpr std::string_view extension = ".bsdiff";
#endif
    return std::format("AltMan-Delta-{}-to-{}{}", fromVersion, toVersion, extension);
}

std::filesystem::path AutoUpdater::GetUpdateScriptPath() {
#ifdef _WIN32
    return std::filesystem::temp_directory_path() / "update_altman.bat";
#else
    return std::filesystem::temp_directory_path() / "update_altman.sh";
#endif
}

void AutoUpdater::CreateUpdateScript(const std::string& newExecutable, const std::string& currentExecutable, const std::string& backupPath) {
    const auto scriptPath = GetUpdateScriptPath();
    std::ofstream script(scriptPath);

#ifdef _WIN32
    std::format_to(std::ostreambuf_iterator<char>(script),
        "@echo off\n"
        "echo Waiting for application to close...\n"
        "timeout /t 2 /nobreak > nul\n"
        "echo Creating backup...\n"
        "copy /Y \"{}\" \"{}\"\n"
        "echo Installing update...\n"
        "move /Y \"{}\" \"{}\"\n"
        "if errorlevel 1 (\n"
        "    echo Update failed! Restoring backup...\n"
        "    copy /Y \"{}\" \"{}\"\n"
        ")\n"
        "echo Starting application...\n"
        "start \"\" \"{}\"\n"
        "del \"%~f0\"\n",
        currentExecutable, backupPath,
        newExecutable, currentExecutable,
        backupPath, currentExecutable,
        currentExecutable
    );
#else
    std::format_to(std::ostreambuf_iterator<char>(script),
        "#!/bin/bash\n"
        "echo 'Waiting for application to close...'\n"
        "sleep 2\n"
        "echo 'Creating backup...'\n"
        "cp \"{}\" \"{}\"\n"
        "echo 'Installing update...'\n"
        "if mv \"{}\" \"{}\"; then\n"
        "    chmod +x \"{}\"\n"
        "    echo 'Update successful!'\n"
        "else\n"
        "    echo 'Update failed! Restoring backup...'\n"
        "    cp \"{}\" \"{}\"\n"
        "fi\n"
        "echo 'Starting application...'\n"
        "\"{}\" &\n"
        "rm \"$0\"\n",
        currentExecutable, backupPath,
        newExecutable, currentExecutable,
        currentExecutable,
        backupPath, currentExecutable,
        currentExecutable
    );
#endif
    script.close();
    std::filesystem::permissions(scriptPath, std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
}

void AutoUpdater::LaunchUpdateScript() {
    const auto scriptPath = GetUpdateScriptPath();

#ifdef _WIN32
    std::wstring cmdLine = L"cmd.exe /C \"" + scriptPath.wstring() + L"\"";

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (!CreateProcessW(
        nullptr,
        cmdLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    )) {
        return;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    pid_t pid;
    std::string path = scriptPath.string();
    char* argv[] = {
        const_cast<char*>(path.c_str()),
        nullptr
    };

    if (posix_spawn(
            &pid,
            path.c_str(),
            nullptr,
            nullptr,
            argv,
            environ
        ) != 0) {
        }
#endif
}

std::string AutoUpdater::FormatBytes(size_t bytes) noexcept {
    constexpr std::array units = {"B", "KB", "MB", "GB", "TB", "PB"};
    size_t unitIndex = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unitIndex + 1 < units.size()) {
        size /= 1024.0;
        ++unitIndex;
    }

    if (unitIndex == 0) {
        return std::format("{} {}", bytes, units[0]);
    }
    return std::format("{:.2f} {}", size, units[unitIndex]);
}

std::string AutoUpdater::FormatSpeed(size_t bytesPerSecond) noexcept {
    return std::format("{}/s", FormatBytes(bytesPerSecond));
}

bool AutoUpdater::ApplyDeltaPatch(const std::filesystem::path& oldFile, const std::filesystem::path& patchFile, const std::filesystem::path& newFile) {
    LOG_INFO("Applying delta patch...");

#ifdef _WIN32
    const auto cmd = std::format("xdelta3 -d -s \"{}\" \"{}\" \"{}\"",
        oldFile.string(), patchFile.string(), newFile.string());
#else
    const auto cmd = std::format("bspatch \"{}\" \"{}\" \"{}\"",
        oldFile.string(), newFile.string(), patchFile.string());
#endif

    return std::system(cmd.c_str()) == 0;
}

UpdateInfo AutoUpdater::ParseReleaseInfo(const nlohmann::json& release, UpdateChannel channel) {
    UpdateInfo info;
    info.version = release.value("tag_name", "");

    if (!info.version.empty() && (info.version.front() == 'v' || info.version.front() == 'V')) {
        info.version = info.version.substr(1);
    }

    info.changelog = release.value("body", "");
    info.channel = channel;
    info.isCritical = (info.changelog.find("[CRITICAL]") != std::string::npos ||
                      info.changelog.find("[SECURITY]") != std::string::npos);

    const auto assetName = GetPlatformAssetName(channel);
    const auto deltaName = GetDeltaAssetName(APP_VERSION, info.version);

    if (release.contains("assets")) {
        for (const auto& asset : release["assets"]) {
            const std::string name = asset.value("name", "");

            if (name == assetName) {
                info.downloadUrl = asset.value("browser_download_url", "");
                info.fullSize = asset.value("size", 0);
            } else if (name == deltaName) {
                info.deltaUrl = asset.value("browser_download_url", "");
                info.deltaSize = asset.value("size", 0);
            }
        }
    }

    return info;
}

std::string AutoUpdater::GetReleaseEndpoint(UpdateChannel channel) {
    constexpr std::string_view base = "https://api.github.com/repos/TheRouletteBoi/altman/releases";

    switch (channel) {
        case UpdateChannel::Beta:
            return std::format("{}?per_page=10", base);
        case UpdateChannel::Dev:
            return std::format("{}?per_page=20", base);
        default:
            return std::format("{}/latest", base);
    }
}

bool AutoUpdater::MatchesChannel(const nlohmann::json& release, UpdateChannel channel) {
    const bool isPrerelease = release.value("prerelease", false);
    const std::string tag = release.value("tag_name", "");

    switch (channel) {
        case UpdateChannel::Stable:
            return !isPrerelease;
        case UpdateChannel::Beta:
            return isPrerelease && (tag.find("beta") != std::string::npos);
        case UpdateChannel::Dev:
            return isPrerelease && (tag.find("dev") != std::string::npos ||
                                   tag.find("alpha") != std::string::npos);
    }
    return false;
}

void AutoUpdater::Initialize() {
    config.Load();

    if (config.autoCheck) {
        StartBackgroundChecker();
    }
}

void AutoUpdater::SetUpdateChannel(UpdateChannel channel) {
    config.channel = channel;
    config.Save();
    LOG_INFO("Update channel set to: {}", GetChannelName(channel));
}

UpdateChannel AutoUpdater::GetUpdateChannel() noexcept {
    return config.channel;
}

void AutoUpdater::SetAutoUpdate(bool autoCheck, bool autoDownload, bool autoInstall) {
    config.autoCheck = autoCheck;
    config.autoDownload = autoDownload;
    config.autoInstall = autoInstall;
    config.Save();

    if (autoCheck) {
        StartBackgroundChecker();
    }
}

void AutoUpdater::SetBandwidthLimit(size_t bytesPerSecond) {
    config.bandwidthLimit = bytesPerSecond;
    config.Save();
    LOG_INFO("Bandwidth limit set to: {}", FormatSpeed(bytesPerSecond));
}

void AutoUpdater::SetShowNotifications(bool show) noexcept {
    config.showNotifications = show;
    config.Save();
}

void AutoUpdater::PauseDownload() noexcept {
    currentDownload.isPaused.store(true);
}

void AutoUpdater::ResumeDownload() noexcept {
    currentDownload.isPaused.store(false);
}

void AutoUpdater::CancelDownload() noexcept {
    currentDownload.shouldCancel.store(true);
}

const DownloadState& AutoUpdater::GetDownloadState() noexcept {
    return currentDownload;
}

void AutoUpdater::StartBackgroundChecker() {
    ThreadTask::fireAndForget([]() {
        while (config.autoCheck) {
            const auto now = std::chrono::system_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::hours>(now - config.lastCheck);

            if (elapsed.count() >= 24) {
                CheckForUpdates(true);
                config.lastCheck = now;
                config.Save();
            }

            std::this_thread::sleep_for(std::chrono::hours(1));
        }
    });
}

void AutoUpdater::CheckForUpdates(bool silent) {
    ThreadTask::fireAndForget([silent]() {
        const auto endpoint = GetReleaseEndpoint(config.channel);
        const auto resp = HttpClient::get(endpoint, {
            {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36"},
            {"Accept", "application/vnd.github+json"}
        });

        if (resp.status_code != 200) {
            LOG_INFO("Failed to check for updates: HTTP {}", resp.status_code);

            if (!silent) {
                const auto showError = []() {
                    if (config.showNotifications) {
                        UpdateNotification::Show("Update Check Failed",
                            "Failed to check for updates. Please try again later.");
                    } else {
                        ModalPopup::AddInfo("Failed to check for updates. Please try again later.");
                    }
                };
                ThreadTask::RunOnMain(showError);
            }
            return;
        }

        const nlohmann::json releases = HttpClient::decode(resp);
        UpdateInfo updateInfo;
        bool foundUpdate = false;

        if (config.channel == UpdateChannel::Stable && releases.is_object()) {
            updateInfo = AutoUpdater::ParseReleaseInfo(releases, config.channel);
            foundUpdate = (!updateInfo.version.empty() && updateInfo.version != APP_VERSION);
        } else if (releases.is_array()) {
            for (const auto& release : releases) {
                if (AutoUpdater::MatchesChannel(release, config.channel)) {
                    updateInfo = AutoUpdater::ParseReleaseInfo(release, config.channel);
                    if (!updateInfo.version.empty() && updateInfo.version != APP_VERSION) {
                        foundUpdate = true;
                        break;
                    }
                }
            }
        }

        if (!foundUpdate) {
            if (!silent) {
                const auto showUpToDate = []() {
                    if (config.showNotifications) {
                        UpdateNotification::Show("Up to Date",
                            "You're using the latest version!");
                    } else {
                        ModalPopup::AddInfo("You're using the latest version!");
                    }
                };
                ThreadTask::RunOnMain(showUpToDate);
            }
            return;
        }

        ThreadTask::RunOnMain([updateInfo, silent]() {
            HandleUpdateAvailable(updateInfo, silent);
        });
    });
}

void AutoUpdater::HandleUpdateAvailable(const UpdateInfo& info, bool silent) {
    const auto channelLabel = std::format(" ({})", GetChannelName(info.channel));
    std::string msg = std::format("Version {}{} is available!", info.version, channelLabel);

    if (info.isCritical) {
        msg = std::format("[CRITICAL UPDATE] {}", msg);
    }

    if (!info.deltaUrl.empty() && info.deltaSize < info.fullSize) {
        const size_t savingsMB = (info.fullSize - info.deltaSize) / (1024 * 1024);
        msg = std::format("{}\n\nDelta update available (saves {} MB)", msg, savingsMB);
    }

    if (!info.changelog.empty() && info.changelog.length() < 200) {
        msg = std::format("{}\n\n{}", msg, info.changelog);
    }

    if (config.showNotifications && !info.isCritical) {
        UpdateNotification::Show("Update Available", msg, 10.0f, [info]() {
            ModalPopup::AddYesNo("Download and install update?", [info]() {
                DownloadAndInstallUpdate(info);
            });
        });
    } else {
        if (config.autoDownload && !silent) {
            msg = std::format("{}\n\nDownload automatically?", msg);
            ModalPopup::AddOk(msg, [info]() {
                DownloadAndInstallUpdate(info);
            });
        } else if (config.autoDownload && config.autoInstall && silent) {
            DownloadAndInstallUpdate(info, true);
        } else if (!silent) {
            ModalPopup::AddOk(msg, [info]() {
                DownloadAndInstallUpdate(info);
            });
        }
    }
}

void AutoUpdater::DownloadAndInstallUpdate(const UpdateInfo& info, bool autoInstall) {
    ThreadTask::fireAndForget([info, autoInstall]() {
        const auto tempPath = std::filesystem::temp_directory_path() / "altman_update.tmp";
        bool useDelta = !info.deltaUrl.empty();
        bool success = false;

        const auto showDownloadStart = [useDelta]() {
            if (config.showNotifications) {
                UpdateNotification::Show("Download Started",
                    useDelta ? "Downloading delta update..." : "Downloading update...",
                    3.0f);
            }
        };
        ThreadTask::RunOnMain(showDownloadStart);

        if (useDelta) {
            const auto patchPath = std::filesystem::temp_directory_path() / "altman_update.patch";

            const auto progressCallback = [](int percentage, size_t speed, size_t total) {
                // LOG_INFO("Download progress: {}% - {} - {}", percentage, AutoUpdater::FormatSpeed(speed), AutoUpdater::FormatBytes(total));
            };

            success = DownloadFileWithResume(info.deltaUrl, patchPath, progressCallback);

            if (success) {
                const auto showPatchApply = []() {
                    if (config.showNotifications) {
                        UpdateNotification::Show("Applying Patch",
                            "Applying delta patch...", 3.0f);
                    }
                };
                ThreadTask::RunOnMain(showPatchApply);

                const auto currentExe = GetCurrentExecutablePath();
                success = ApplyDeltaPatch(currentExe, patchPath, tempPath);
                std::filesystem::remove(patchPath);

                if (!success) {
                    LOG_WARN("Delta patch failed, falling back to full download");
                    useDelta = false;
                }
            }
        }

        if (!useDelta) {
            const auto progressCallback = [](int percentage, size_t speed, size_t total) {
                // LOG_INFO("Download progress: {}% - {} - {}", percentage, AutoUpdater::FormatSpeed(speed), AutoUpdater::FormatBytes(total));
            };

            success = DownloadFileWithResume(info.downloadUrl, tempPath, progressCallback);
        }

        if (!success) {
            const auto showError = []() {
                if (config.showNotifications) {
                    UpdateNotification::Show("Download Failed",
                        "Download failed. Please try again later.", 5.0f);
                } else {
                    ModalPopup::AddInfo("Download failed. Please try again later.");
                }
            };
            ThreadTask::RunOnMain(showError);
            return;
        }

        const auto showComplete = []() {
            if (config.showNotifications) {
                UpdateNotification::Show("Download Complete",
                    "Update downloaded successfully!", 3.0f);
            }
        };
        ThreadTask::RunOnMain(showComplete);

        pendingUpdatePath = tempPath;
        config.lastInstalledVersion = info.version;

        if (autoInstall || config.autoInstall) {
            ThreadTask::RunOnMain([tempPath]() {
                InstallUpdate(tempPath);
            });
        } else {
            if (config.showNotifications) {
                ThreadTask::RunOnMain([tempPath]() {
                    UpdateNotification::ShowPersistent("Update Ready",
                        "Click to install and restart", [tempPath]() {
                            InstallUpdate(tempPath);
                        });
                });
            } else {
                ThreadTask::RunOnMain([tempPath]() {
                    ModalPopup::AddYesNo("Update ready! Restart now to install?",
                        [tempPath]() {
                            InstallUpdate(tempPath);
                        });
                });
            }
        }
    });
}

bool AutoUpdater::DownloadFileWithResume(std::string_view url, const std::filesystem::path& outputPath,
                                  std::function<void(int, size_t, size_t)> progressCallback) {
    LOG_INFO("Downloading: {}", url);

    currentDownload.url = std::string(url);
    currentDownload.outputPath = outputPath.string();
    currentDownload.isPaused.store(false);
    currentDownload.shouldCancel.store(false);
    currentDownload.isComplete.store(false);
    currentDownload.downloadedBytes.store(0);
    currentDownload.totalBytes.store(0);
    currentDownload.startTime = std::chrono::steady_clock::now();
    currentDownload.lastUpdateTime = currentDownload.startTime;

    size_t startOffset = 0;
    if (config.resumeFilePath == outputPath && config.resumeOffset > 0) {
        if (std::filesystem::exists(outputPath)) {
            startOffset = config.resumeOffset;
            LOG_INFO("Resuming download from byte {}", startOffset);
        }
    } else if (std::filesystem::exists(outputPath)) {
        std::filesystem::remove(outputPath);
    }

    std::vector<std::pair<std::string, std::string>> headers = {
        {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36"}
    };

    HttpClient::DownloadControl control{
        .shouldCancel = &currentDownload.shouldCancel,
        .isPaused = &currentDownload.isPaused,
        .bandwidthLimit = config.bandwidthLimit
    };

    HttpClient::ExtendedProgressCallback extendedProgress = nullptr;
    if (progressCallback) {
        extendedProgress = [progressCallback](size_t downloaded, size_t total, size_t bytesPerSecond) {
            currentDownload.downloadedBytes.store(downloaded);
            currentDownload.totalBytes.store(total);
            currentDownload.lastUpdateTime = std::chrono::steady_clock::now();

            const int percentage = total > 0
                ? static_cast<int>((downloaded * 100) / total)
                : 0;

            ThreadTask::RunOnMain([progressCallback, percentage, bytesPerSecond, total]() {
                progressCallback(percentage, bytesPerSecond, total);
            });
        };
    }

    auto result = HttpClient::download_streaming(
        std::string(url),
        outputPath.string(),
        headers,
        startOffset,
        extendedProgress,
        control
    );

    if (result.wasCancelled) {
        config.resumeFilePath = outputPath;
        config.resumeOffset = result.bytesDownloaded;
        config.Save();
        LOG_INFO("Download cancelled, progress saved for resume at byte {}", result.bytesDownloaded);
        return false;
    }

    if (!result.error.empty()) {
        LOG_ERROR("Download failed: {}", result.error);
        return false;
    }

    if (result.status_code != 200 && result.status_code != 206) {
        LOG_ERROR("Download failed: HTTP {}", result.status_code);
        return false;
    }

    currentDownload.isComplete.store(true);
    currentDownload.downloadedBytes.store(result.bytesDownloaded);
    currentDownload.totalBytes.store(result.totalBytes);

    config.resumeFilePath.clear();
    config.resumeOffset = 0;
    config.Save();

    if (progressCallback) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - currentDownload.startTime).count();
        const size_t bytesPerSecond = elapsed > 0
            ? (result.bytesDownloaded - startOffset) / static_cast<size_t>(elapsed)
            : 0;

        ThreadTask::RunOnMain([progressCallback, bytesPerSecond, total = result.totalBytes]() {
            progressCallback(100, bytesPerSecond, total);
        });
    }

    LOG_INFO("Download complete: {} ({} bytes)", outputPath.string(), result.bytesDownloaded);
    return true;
}

void AutoUpdater::InstallUpdate(const std::filesystem::path& updatePath) {
    const auto currentExe = GetCurrentExecutablePath();
    auto backupPath = AltMan::Paths::BackupFile(std::format("altman_v{}_backup", APP_VERSION));

#ifdef _WIN32
    backupPath.replace_extension(".exe");
#endif

    config.backupPath = backupPath;
    config.Save();

    CreateUpdateScript(updatePath.string(), currentExe.string(), backupPath.string());
    LaunchUpdateScript();

    std::exit(0);
}

void AutoUpdater::RollbackToPreviousVersion() {
    if (config.backupPath.empty() || !std::filesystem::exists(config.backupPath)) {
        LOG_INFO("No backup available for rollback");

        const auto showError = []() {
            if (config.showNotifications) {
                UpdateNotification::Show("Rollback Failed",
                    "No backup found. Cannot rollback.", 5.0f);
            } else {
                ModalPopup::AddInfo("No backup found. Cannot rollback.");
            }
        };
        ThreadTask::RunOnMain(showError);
        return;
    }

    ThreadTask::fireAndForget([]() {
        const auto currentExe = GetCurrentExecutablePath();
        const auto tempBackup = std::filesystem::path(currentExe).concat(".rollback_tmp");

        std::filesystem::copy_file(currentExe, tempBackup, std::filesystem::copy_options::overwrite_existing);
        CreateUpdateScript(config.backupPath.string(), currentExe.string(), tempBackup.string());

        const auto confirmRollback = []() {
            ModalPopup::AddYesNo("Rolling back to previous version. Restart now?",
                []() {
                    LaunchUpdateScript();
                    std::exit(0);
                });
        };
        ThreadTask::RunOnMain(confirmRollback);
    });
}

void AutoUpdater::CleanupOldBackups(int keepCount) {
    const auto backupDir = AltMan::Paths::Backups();

    std::vector<std::filesystem::path> backups;

    for (const auto& entry : std::filesystem::directory_iterator(backupDir)) {
        if (entry.path().filename().string().find("altman_v") != std::string::npos) {
            backups.push_back(entry.path());
        }
    }

    if (backups.size() <= static_cast<size_t>(keepCount)) {
        return;
    }

    std::ranges::sort(backups, [](const std::filesystem::path& a, const std::filesystem::path& b) {
        return std::filesystem::last_write_time(a) > std::filesystem::last_write_time(b);
    });

    for (size_t i = static_cast<size_t>(keepCount); i < backups.size(); ++i) {
        std::filesystem::remove(backups[i]);
        LOG_INFO("Removed old backup: {}", backups[i].string());
    }
}

std::filesystem::path AutoUpdater::GetCurrentExecutablePath() {
#ifdef _WIN32
    std::array<char, MAX_PATH> buffer{};
    GetModuleFileNameA(nullptr, buffer.data(), MAX_PATH);
    return std::filesystem::path(buffer.data());
#elif __APPLE__
    std::array<char, 1024> buffer{};
    uint32_t size = buffer.size();
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
        return std::filesystem::path(buffer.data());
    }
    return {};
#else
    return std::filesystem::read_symlink("/proc/self/exe");
#endif
}

#ifdef __APPLE__
std::string GetApplicationNameFromBundleId() {
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    if (!mainBundle) return "";

    auto nameRef = static_cast<CFStringRef>(
        CFBundleGetValueForInfoDictionaryKey(mainBundle, kCFBundleNameKey));
    if (!nameRef) return "";

    std::array<char, 256> buffer{};
    if (!CFStringGetCString(nameRef, buffer.data(), buffer.size(), kCFStringEncodingUTF8)) {
        return "";
    }
    return std::string(buffer.data());
}

std::string GetCurrentExecutableRealPath() {
    std::array<char, PATH_MAX> buffer{};
    uint32_t size = buffer.size();

    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return "";
    }

    std::array<char, PATH_MAX> resolved{};
    if (!realpath(buffer.data(), resolved.data())) {
        return "";
    }
    return std::string(resolved.data());
}
#endif
