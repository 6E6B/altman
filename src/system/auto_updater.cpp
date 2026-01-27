#include "auto_updater.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <print>
#include <ranges>
#include <thread>

#include "console/console.h"
#include "network/http.h"
#include "system/system_info.h"
#include "ui/widgets/modal_popup.h"
#include "ui/widgets/notifications.h"
#include "ui/widgets/progress_overlay.h"
#include "utils/paths.h"
#include "utils/thread_task.h"
#include "version.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else

#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/stat.h>
extern char **environ;
#endif

#endif

std::filesystem::path UpdaterConfig::GetConfigPath() {
    return AltMan::Paths::Config("updater.json");
}

void UpdaterConfig::Save() const {
    const nlohmann::json j = {
        {"channel",              static_cast<int>(channel)                      },
        {"autoCheck",            autoCheck                                      },
        {"autoDownload",         autoDownload                                   },
        {"autoInstall",          autoInstall                                    },
        {"bandwidthLimit",       bandwidthLimit                                 },
        {"lastCheck",            std::chrono::system_clock::to_time_t(lastCheck)},
        {"lastInstalledVersion", lastInstalledVersion                           },
        {"backupPath",           backupPath.string()                            },
        {"resumeFilePath",       resumeFilePath.string()                        },
        {"resumeOffset",         resumeOffset                                   }
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
    constexpr std::string_view UPDATE_TASK_ID = "auto_update_download";
} // namespace

constexpr std::string_view AutoUpdater::GetChannelName(UpdateChannel channel) noexcept {
    switch (channel) {
        case UpdateChannel::Beta:
            return "beta";
        case UpdateChannel::Dev:
            return "dev";
        default:
            return "stable";
    }
}

std::string AutoUpdater::GetPlatformAssetName(UpdateChannel channel) {
    // Asset naming:
    // Windows: AltMan-Windows-x86_64.exe, AltMan-Windows-arm64.exe
    // macOS:   AltMan-macOS.zip (universal binary)
    // Beta:    AltMan-Windows-x86_64-beta.exe, AltMan-macOS-beta.zip

    const auto platform = SystemInfo::GetPlatformString();

#ifdef _WIN32
    const auto arch = SystemInfo::GetArchitectureString();
    if (channel == UpdateChannel::Stable) {
        return std::format("AltMan-{}-{}.exe", platform, arch);
    }
    return std::format("AltMan-{}-{}-{}.exe", platform, arch, GetChannelName(channel));
#else
    // macOS ships universal binary - no arch suffix
    if (channel == UpdateChannel::Stable) {
        return std::format("AltMan-{}.zip", platform);
    }
    return std::format("AltMan-{}-{}.zip", platform, GetChannelName(channel));
#endif
}

std::string AutoUpdater::GetDeltaAssetName(std::string_view fromVersion, std::string_view toVersion) {
    // Delta naming:
    // Windows: AltMan-Delta-1.0.0-to-1.1.0-Windows-x86_64.xdelta
    // macOS:   Called separately for each arch - this function is only used on Windows
    //          macOS uses GetDeltaAssetNameForArch() internally

    const auto platform = SystemInfo::GetPlatformString();
    const auto arch = SystemInfo::GetArchitectureString();

#ifdef _WIN32
    return std::format("AltMan-Delta-{}-to-{}-{}-{}.xdelta", fromVersion, toVersion, platform, arch);
#else
    // This shouldn't be called on macOS - use ParseReleaseInfo which handles both archs
    return std::format("AltMan-Delta-{}-to-{}-{}-arm64.bsdiff", fromVersion, toVersion, platform);
#endif
}

std::filesystem::path AutoUpdater::GetUpdateScriptPath() {
#ifdef _WIN32
    return std::filesystem::temp_directory_path() / "update_altman.ps1";
#else
    return std::filesystem::temp_directory_path() / "update_altman.sh";
#endif
}

std::filesystem::path AutoUpdater::GetCurrentExecutablePath() {
#ifdef _WIN32
    std::array<wchar_t, MAX_PATH> buffer {};
    GetModuleFileNameW(nullptr, buffer.data(), MAX_PATH);
    return std::filesystem::path(buffer.data());
#elif __APPLE__
    std::array<char, 1024> buffer {};
    uint32_t size = buffer.size();
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
        std::array<char, PATH_MAX> resolved {};
        if (realpath(buffer.data(), resolved.data())) {
            return std::filesystem::path(resolved.data());
        }
        return std::filesystem::path(buffer.data());
    }
    return {};
#else
    return {};
#endif
}

std::filesystem::path AutoUpdater::GetAppBundlePath() {
#ifdef __APPLE__
    auto exePath = GetCurrentExecutablePath();
    // Navigate from Contents/MacOS/AppName up to AppName.app
    // /path/to/App.app/Contents/MacOS/AppName -> /path/to/App.app
    auto path = exePath.parent_path().parent_path().parent_path();
    if (path.extension() == ".app") {
        return path;
    }
    // Fallback: might be running outside a bundle
    return exePath;
#else
    return GetCurrentExecutablePath();
#endif
}

void AutoUpdater::CreateUpdateScript(
    const std::string &newPath,
    const std::string &currentPath,
    const std::string &backupPath
) {
    const auto scriptPath = GetUpdateScriptPath();
    std::ofstream script(scriptPath);

#ifdef _WIN32
    std::format_to(
        std::ostreambuf_iterator<char>(script),
        "$ErrorActionPreference = 'Stop'\n"
        "$LogFile = \"$env:TEMP\\altman_update.log\"\n"
        "\n"
        "try {{\n"
        "    Start-Sleep -Seconds 2\n"
        "\n"
        "    $NewPath = '{}'\n"
        "    $CurrentPath = '{}'\n"
        "    $BackupPath = '{}'\n"
        "\n"
        "    $BackupDir = Split-Path -Parent $BackupPath\n"
        "    if (-not (Test-Path $BackupDir)) {{\n"
        "        New-Item -ItemType Directory -Path $BackupDir -Force | Out-Null\n"
        "    }}\n"
        "\n"
        "    Copy-Item -Path $CurrentPath -Destination $BackupPath -Force\n"
        "    Move-Item -Path $NewPath -Destination $CurrentPath -Force\n"
        "    Start-Process -FilePath $CurrentPath\n"
        "    Remove-Item -Path $MyInvocation.MyCommand.Source -Force\n"
        "}}\n"
        "catch {{\n"
        "    $_.Exception.Message | Out-File -FilePath $LogFile\n"
        "}}\n",
        newPath,
        currentPath,
        backupPath
    );
#else
    std::format_to(
        std::ostreambuf_iterator<char>(script),
        "#!/bin/bash\n"
        "set -e\n"
        "\n"
        "LOG_FILE=\"/tmp/altman_update.log\"\n"
        "exec > \"$LOG_FILE\" 2>&1\n"
        "\n"
        "echo \"Update script started at $(date)\"\n"
        "echo 'Waiting for application to close...'\n"
        "sleep 2\n"
        "\n"
        "NEW_PATH=\"{}\"\n"
        "CURRENT_PATH=\"{}\"\n"
        "BACKUP_PATH=\"{}\"\n"
        "\n"
        "echo \"NEW_PATH: $NEW_PATH\"\n"
        "echo \"CURRENT_PATH: $CURRENT_PATH\"\n"
        "echo \"BACKUP_PATH: $BACKUP_PATH\"\n"
        "\n"
        "echo 'Creating backup...'\n"
        "if [[ -d \"$CURRENT_PATH\" ]]; then\n"
        "    cp -R \"$CURRENT_PATH\" \"$BACKUP_PATH\"\n"
        "else\n"
        "    cp \"$CURRENT_PATH\" \"$BACKUP_PATH\"\n"
        "fi\n"
        "\n"
        "echo 'Installing update...'\n"
        "if [[ -d \"$NEW_PATH\" ]]; then\n"
        "    rm -rf \"$CURRENT_PATH\"\n"
        "    mv \"$NEW_PATH\" \"$CURRENT_PATH\"\n"
        "else\n"
        "    mv \"$NEW_PATH\" \"$CURRENT_PATH\"\n"
        "fi\n"
        "\n"
        "echo 'Code signing...'\n"
        "if [[ -d \"$CURRENT_PATH\" ]]; then\n"
        "    codesign --force --deep --sign - \"$CURRENT_PATH\" 2>/dev/null || true\n"
        "else\n"
        "    codesign --force --sign - \"$CURRENT_PATH\" 2>/dev/null || true\n"
        "fi\n"
        "\n"
        "xattr -rd com.apple.quarantine \"$CURRENT_PATH\" 2>/dev/null || true\n"
        "\n"
        "echo 'Update successful!'\n"
        "echo 'Starting application...'\n"
        "if [[ -d \"$CURRENT_PATH\" ]]; then\n"
        "    open \"$CURRENT_PATH\"\n"
        "else\n"
        "    \"$CURRENT_PATH\" &\n"
        "fi\n"
        "\n"
        "rm \"$0\"\n",
        newPath,
        currentPath,
        backupPath
    );
#endif

    script.close();

#ifndef _WIN32
    std::filesystem::permissions(
        scriptPath,
        std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
    );
#endif
}

void AutoUpdater::LaunchUpdateScript() {
    const auto scriptPath = GetUpdateScriptPath();

#ifdef _WIN32
    SHELLEXECUTEINFOW sei {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = L"powershell.exe";

    std::wstring args
        = L"-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File \"" + scriptPath.wstring() + L"\"";
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        LOG_ERROR("Failed to launch update script: {}", GetLastError());
        return;
    }

    if (sei.hProcess) {
        CloseHandle(sei.hProcess);
    }
#else
    pid_t pid;
    std::string path = scriptPath.string();
    char *argv[] = {const_cast<char *>("/bin/bash"), const_cast<char *>(path.c_str()), nullptr};

    if (posix_spawn(&pid, "/bin/bash", nullptr, nullptr, argv, environ) != 0) {
        LOG_ERROR("Failed to launch update script");
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

#ifdef __APPLE__
bool AutoUpdater::ExtractZipToPath(const std::filesystem::path &zipPath, const std::filesystem::path &destPath) {
    LOG_INFO("Extracting {} to {}", zipPath.string(), destPath.string());

    std::filesystem::create_directories(destPath);

    // Use ditto for extraction (preserves resource forks and metadata)
    const auto cmd = std::format("ditto -xk \"{}\" \"{}\"", zipPath.string(), destPath.string());
    const int result = std::system(cmd.c_str());

    if (result != 0) {
        LOG_ERROR("Failed to extract zip: exit code {}", result);
        return false;
    }

    return true;
}

bool AutoUpdater::IsUniversalBinary(const std::filesystem::path &binaryPath) {
    const auto cmd
        = std::format("lipo -info \"{}\" 2>/dev/null | grep -q 'Architectures in the fat file'", binaryPath.string());
    return std::system(cmd.c_str()) == 0;
}

bool AutoUpdater::ExtractSlice(
    const std::filesystem::path &binaryPath,
    const std::string &arch,
    const std::filesystem::path &outputPath
) {
    LOG_INFO("Extracting {} slice from {}", arch, binaryPath.string());

    const auto cmd = std::format("lipo \"{}\" -thin {} -output \"{}\"", binaryPath.string(), arch, outputPath.string());

    const int result = std::system(cmd.c_str());
    if (result != 0) {
        LOG_ERROR("Failed to extract {} slice: exit code {}", arch, result);
        return false;
    }

    return true;
}

bool AutoUpdater::CreateUniversalBinary(
    const std::filesystem::path &arm64Path,
    const std::filesystem::path &x86_64Path,
    const std::filesystem::path &outputPath
) {
    LOG_INFO("Creating universal binary at {}", outputPath.string());

    const auto cmd = std::format(
        "lipo -create \"{}\" \"{}\" -output \"{}\"",
        arm64Path.string(),
        x86_64Path.string(),
        outputPath.string()
    );

    const int result = std::system(cmd.c_str());
    if (result != 0) {
        LOG_ERROR("Failed to create universal binary: exit code {}", result);
        return false;
    }

    chmod(outputPath.string().c_str(), 0755);

    return true;
}

bool AutoUpdater::ApplyUniversalDeltaUpdate(
    const std::filesystem::path &arm64PatchPath,
    const std::filesystem::path &x86_64PatchPath,
    const std::filesystem::path &outputAppPath
) {
    LOG_INFO("Applying universal binary delta update...");

    const auto currentAppPath = GetAppBundlePath();
    const auto currentBinaryPath = currentAppPath / "Contents" / "MacOS" / "AltMan";

    if (!std::filesystem::exists(currentBinaryPath)) {
        LOG_ERROR("Current binary not found: {}", currentBinaryPath.string());
        return false;
    }

    if (!IsUniversalBinary(currentBinaryPath)) {
        LOG_ERROR("Current binary is not a universal binary");
        return false;
    }

    const auto tempDir = std::filesystem::temp_directory_path() / "altman_delta_work";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);

    // Extract both slices from current binary
    const auto currentArm64 = tempDir / "current_arm64";
    const auto currentX86_64 = tempDir / "current_x86_64";

    if (!ExtractSlice(currentBinaryPath, "arm64", currentArm64)) {
        LOG_ERROR("Failed to extract arm64 slice from current binary");
        std::filesystem::remove_all(tempDir);
        return false;
    }

    if (!ExtractSlice(currentBinaryPath, "x86_64", currentX86_64)) {
        LOG_ERROR("Failed to extract x86_64 slice from current binary");
        std::filesystem::remove_all(tempDir);
        return false;
    }

    const auto patchedArm64 = tempDir / "patched_arm64";
    const auto patchedX86_64 = tempDir / "patched_x86_64";

    LOG_INFO("Patching arm64 slice...");
    if (!ApplyDeltaPatch(currentArm64, arm64PatchPath, patchedArm64)) {
        LOG_ERROR("Failed to patch arm64 slice");
        std::filesystem::remove_all(tempDir);
        return false;
    }

    LOG_INFO("Patching x86_64 slice...");
    if (!ApplyDeltaPatch(currentX86_64, x86_64PatchPath, patchedX86_64)) {
        LOG_ERROR("Failed to patch x86_64 slice");
        std::filesystem::remove_all(tempDir);
        return false;
    }

    LOG_INFO("Copying app bundle to output...");
    std::filesystem::copy(currentAppPath, outputAppPath, std::filesystem::copy_options::recursive);

    // Reassemble universal binary from patched slices
    const auto outputBinaryPath = outputAppPath / "Contents" / "MacOS" / "AltMan";
    std::filesystem::remove(outputBinaryPath);

    if (!CreateUniversalBinary(patchedArm64, patchedX86_64, outputBinaryPath)) {
        LOG_ERROR("Failed to reassemble universal binary");
        std::filesystem::remove_all(tempDir);
        std::filesystem::remove_all(outputAppPath);
        return false;
    }

    std::filesystem::remove_all(tempDir);

    LOG_INFO("Universal binary delta update applied successfully");
    return true;
}
#endif

bool AutoUpdater::ApplyDeltaPatch(
    const std::filesystem::path &oldFile,
    const std::filesystem::path &patchFile,
    const std::filesystem::path &newFile
) {
    LOG_INFO("Applying delta patch...");
    LOG_INFO("  Old: {}", oldFile.string());
    LOG_INFO("  Patch: {}", patchFile.string());
    LOG_INFO("  New: {}", newFile.string());

#ifdef _WIN32
    // Windows: Use xdelta3
    // xdelta3 -d -s <old> <patch> <new>
    const auto cmd
        = std::format("xdelta3 -d -s \"{}\" \"{}\" \"{}\"", oldFile.string(), patchFile.string(), newFile.string());
#else
    // macOS: Use bspatch
    // bspatch <old> <new> <patch>
    const auto cmd
        = std::format("bspatch \"{}\" \"{}\" \"{}\"", oldFile.string(), newFile.string(), patchFile.string());
#endif

    LOG_INFO("Running: {}", cmd);
    const int result = std::system(cmd.c_str());

    if (result != 0) {
        LOG_ERROR("Delta patch failed with exit code: {}", result);
        return false;
    }

    LOG_INFO("Delta patch applied successfully");
    return true;
}

UpdateInfo AutoUpdater::ParseReleaseInfo(const nlohmann::json &release, UpdateChannel channel) {
    UpdateInfo info;
    info.version = release.value("tag_name", "");

    if (!info.version.empty() && (info.version.front() == 'v' || info.version.front() == 'V')) {
        info.version = info.version.substr(1);
    }

    info.changelog = release.value("body", "");
    info.channel = channel;
    info.isCritical
        = (info.changelog.find("[CRITICAL]") != std::string::npos
           || info.changelog.find("[SECURITY]") != std::string::npos);

    const auto fullAssetName = GetPlatformAssetName(channel);

#ifdef _WIN32
    const auto deltaAssetName = GetDeltaAssetName(APP_VERSION, info.version);
#else
    // macOS: look for both arm64 and x86_64 delta patches
    const auto deltaAssetName_arm64
        = std::format("AltMan-Delta-{}-to-{}-macOS-arm64.bsdiff", APP_VERSION, info.version);
    const auto deltaAssetName_x86_64
        = std::format("AltMan-Delta-{}-to-{}-macOS-x86_64.bsdiff", APP_VERSION, info.version);
#endif

    LOG_INFO("Looking for full asset: '{}'", fullAssetName);
#ifdef _WIN32
    LOG_INFO("Looking for delta asset: '{}'", deltaAssetName);
#else
    LOG_INFO("Looking for delta assets: '{}', '{}'", deltaAssetName_arm64, deltaAssetName_x86_64);
#endif

    if (release.contains("assets")) {
        for (const auto &asset: release["assets"]) {
            const std::string name = asset.value("name", "");

            if (name == fullAssetName) {
                info.downloadUrl = asset.value("browser_download_url", "");
                info.fullSize = asset.value("size", 0);
                LOG_INFO("Found full asset: {} ({} bytes)", name, info.fullSize);
            }
#ifdef _WIN32
            else if (name == deltaAssetName) {
                info.deltaUrl = asset.value("browser_download_url", "");
                info.deltaSize = asset.value("size", 0);
                LOG_INFO("Found delta asset: {} ({} bytes)", name, info.deltaSize);
            }
#else
            else if (name == deltaAssetName_arm64) {
                info.deltaUrl_arm64 = asset.value("browser_download_url", "");
                info.deltaSize_arm64 = asset.value("size", 0);
                LOG_INFO("Found arm64 delta: {} ({} bytes)", name, info.deltaSize_arm64);
            } else if (name == deltaAssetName_x86_64) {
                info.deltaUrl_x86_64 = asset.value("browser_download_url", "");
                info.deltaSize_x86_64 = asset.value("size", 0);
                LOG_INFO("Found x86_64 delta: {} ({} bytes)", name, info.deltaSize_x86_64);
            }
#endif
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

bool AutoUpdater::MatchesChannel(const nlohmann::json &release, UpdateChannel channel) {
    const bool isPrerelease = release.value("prerelease", false);
    const std::string tag = release.value("tag_name", "");

    switch (channel) {
        case UpdateChannel::Stable:
            return !isPrerelease;
        case UpdateChannel::Beta:
            return isPrerelease && (tag.find("beta") != std::string::npos);
        case UpdateChannel::Dev:
            return isPrerelease && (tag.find("dev") != std::string::npos || tag.find("alpha") != std::string::npos);
    }
    return false;
}

void AutoUpdater::Initialize() {
    config.Load();

    LOG_INFO("AutoUpdater initialized");
    LOG_INFO("Platform: {}", SystemInfo::GetPlatformString());
    LOG_INFO("Architecture: {}", SystemInfo::GetArchitectureString());
    LOG_INFO("Hardware: {}", SystemInfo::GetHardwareArchitecture());
#ifdef _WIN32
    LOG_INFO("Emulated: {}", SystemInfo::IsRunningUnderEmulation() ? "yes" : "no");
#else
    LOG_INFO("Rosetta: {}", SystemInfo::IsRunningUnderRosetta() ? "yes" : "no");
#endif

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

void AutoUpdater::PauseDownload() noexcept {
    currentDownload.isPaused.store(true);
}

void AutoUpdater::ResumeDownload() noexcept {
    currentDownload.isPaused.store(false);
}

void AutoUpdater::CancelDownload() noexcept {
    currentDownload.shouldCancel.store(true);
}

const DownloadState &AutoUpdater::GetDownloadState() noexcept {
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
        LOG_INFO("Checking for updates (channel: {})", GetChannelName(config.channel));

        const auto endpoint = GetReleaseEndpoint(config.channel);
        const auto resp = HttpClient::get(
            endpoint,
            {
                {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"},
                {"Accept",     "application/vnd.github+json"                                 }
        }
        );

        if (resp.status_code != 200) {
            LOG_ERROR("Failed to check for updates: HTTP {}", resp.status_code);

            if (!silent) {
                ThreadTask::RunOnMain([]() {
                    UpdateNotification::Show(
                        "Update Check Failed",
                        "Failed to check for updates. Please try again later."
                    );
                });
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
            for (const auto &release: releases) {
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
            LOG_INFO("No updates available (current: {})", APP_VERSION);
            if (!silent) {
                ThreadTask::RunOnMain([]() {
                    UpdateNotification::Show("Up to Date", "You're using the latest version!");
                });
            }
            return;
        }

        LOG_INFO("Update available: {} -> {}", APP_VERSION, updateInfo.version);

        ThreadTask::RunOnMain([updateInfo, silent]() {
            HandleUpdateAvailable(updateInfo, silent);
        });
    });
}

void AutoUpdater::HandleUpdateAvailable(const UpdateInfo &info, bool silent) {
    const auto channelLabel
        = (info.channel != UpdateChannel::Stable) ? std::format(" ({})", GetChannelName(info.channel)) : "";

    std::string msg = std::format("Version {}{} is available!", info.version, channelLabel);

    if (info.isCritical) {
        msg = std::format("[CRITICAL UPDATE] {}", msg);
    }

    if (info.hasDelta() && info.totalDeltaSize() < info.fullSize) {
        const size_t savingsMB = (info.fullSize - info.totalDeltaSize()) / (1024 * 1024);
        msg = std::format("{}\n\nDelta update available (saves ~{} MB)", msg, savingsMB);
    }

    if (!info.changelog.empty() && info.changelog.length() < 200) {
        msg = std::format("{}\n\n{}", msg, info.changelog);
    }

    if (!info.isCritical) {
        UpdateNotification::Show("Update Available", msg, 10.0f, [info]() {
            ModalPopup::AddYesNo("Download and install update?", [info]() {
                DownloadAndInstallUpdate(info);
            });
        });
    } else {
        if (config.autoDownload && config.autoInstall && silent) {
            DownloadAndInstallUpdate(info, true);
        } else {
            ModalPopup::AddOk(msg, [info]() {
                DownloadAndInstallUpdate(info);
            });
        }
    }
}

void AutoUpdater::DownloadAndInstallUpdate(const UpdateInfo &info, bool autoInstall) {
    ThreadTask::fireAndForget([info, autoInstall]() {
        const bool useDelta = info.hasDelta();
        bool success = false;

        const auto tempDir = std::filesystem::temp_directory_path() / "altman_update";
        std::filesystem::remove_all(tempDir);
        std::filesystem::create_directories(tempDir);

#ifdef __APPLE__
        const auto outputAppPath = tempDir / "AltMan.app";
#else
        const auto outputExePath = tempDir / "AltMan.exe";
#endif

        ThreadTask::RunOnMain([&info, useDelta]() {
            ProgressOverlay::Add(
                std::string(UPDATE_TASK_ID),
                useDelta ? "Downloading delta update" : "Downloading update",
                true,
                []() {
                    CancelDownload();
                }
            );
            ProgressOverlay::Update(
                std::string(UPDATE_TASK_ID),
                0.0f,
                std::format("0 / {}", FormatBytes(useDelta ? info.totalDeltaSize() : info.fullSize))
            );
        });

        const auto progressCallback = [](int percentage, size_t speed, size_t total) {
            ThreadTask::RunOnMain([percentage, speed, total]() {
                const float progress = static_cast<float>(percentage) / 100.0f;
                const auto detail
                    = std::format("{} • {}", FormatBytes(static_cast<size_t>(progress * total)), FormatSpeed(speed));
                ProgressOverlay::Update(std::string(UPDATE_TASK_ID), progress, detail);
            });
        };

        if (useDelta) {
#ifdef __APPLE__
            const auto arm64PatchPath = tempDir / "patch_arm64.bsdiff";
            const auto x86_64PatchPath = tempDir / "patch_x86_64.bsdiff";

            LOG_INFO("Downloading arm64 delta patch...");
            ThreadTask::RunOnMain([]() {
                UpdateNotification::Show("Downloading", "Downloading arm64 patch...", 2.0f);
            });

            bool arm64Success = DownloadFileWithResume(info.deltaUrl_arm64, arm64PatchPath, progressCallback);

            if (arm64Success) {
                LOG_INFO("Downloading x86_64 delta patch...");
                ThreadTask::RunOnMain([]() {
                    UpdateNotification::Show("Downloading", "Downloading x86_64 patch...", 2.0f);
                });

                bool x86_64Success = DownloadFileWithResume(info.deltaUrl_x86_64, x86_64PatchPath, progressCallback);

                if (x86_64Success) {
                    ThreadTask::RunOnMain([]() {
                        UpdateNotification::Show("Applying Patch", "Applying delta patches...", 3.0f);
                    });

                    success = ApplyUniversalDeltaUpdate(arm64PatchPath, x86_64PatchPath, outputAppPath);

                    std::filesystem::remove(arm64PatchPath);
                    std::filesystem::remove(x86_64PatchPath);
                }
            }

            if (!success) {
                LOG_WARN("Delta patch failed, falling back to full download");
                std::filesystem::remove(arm64PatchPath);
                std::filesystem::remove(x86_64PatchPath);
                if (std::filesystem::exists(outputAppPath)) {
                    std::filesystem::remove_all(outputAppPath);
                }
            }
#else
            const auto patchPath = tempDir / "update.xdelta";
            success = DownloadFileWithResume(info.deltaUrl, patchPath, progressCallback);

            if (success) {
                ThreadTask::RunOnMain([]() {
                    UpdateNotification::Show("Applying Patch", "Applying delta patch...", 3.0f);
                });

                const auto currentExePath = GetCurrentExecutablePath();
                success = ApplyDeltaPatch(currentExePath, patchPath, outputExePath);
                std::filesystem::remove(patchPath);

                if (!success) {
                    LOG_WARN("Delta patch failed, falling back to full download");
                }
            }
#endif
        }

        if (!useDelta || !success) {
#ifdef __APPLE__
            const auto zipPath = tempDir / "update.zip";
            success = DownloadFileWithResume(info.downloadUrl, zipPath, progressCallback);

            if (success) {
                LOG_INFO("Extracting update...");

                const auto extractPath = tempDir / "extracted";
                success = ExtractZipToPath(zipPath, extractPath);
                std::filesystem::remove(zipPath);

                if (success) {
                    for (const auto &entry: std::filesystem::directory_iterator(extractPath)) {
                        if (entry.path().extension() == ".app") {
                            if (std::filesystem::exists(outputAppPath)) {
                                std::filesystem::remove_all(outputAppPath);
                            }
                            std::filesystem::rename(entry.path(), outputAppPath);
                            break;
                        }
                    }

                    success = std::filesystem::exists(outputAppPath);
                }
            }
#else
            success = DownloadFileWithResume(info.downloadUrl, outputExePath, progressCallback);
#endif
        }

        if (!success) {
            ThreadTask::RunOnMain([]() {
                ProgressOverlay::Complete(std::string(UPDATE_TASK_ID), false, "Download failed");
            });
            std::filesystem::remove_all(tempDir);
            return;
        }

        ThreadTask::RunOnMain([]() {
            ProgressOverlay::Complete(std::string(UPDATE_TASK_ID), true, "Ready to install");
        });


#ifdef __APPLE__
        pendingUpdatePath = outputAppPath;
#else
        pendingUpdatePath = outputExePath;
#endif
        config.lastInstalledVersion = info.version;

        if (autoInstall || config.autoInstall) {
            ThreadTask::RunOnMain([]() {
                InstallUpdate(pendingUpdatePath);
            });
        } else {
            ThreadTask::RunOnMain([]() {
                UpdateNotification::ShowPersistent("Update Ready", "Click to install and restart", []() {
                    InstallUpdate(pendingUpdatePath);
                });
            });
        }
    });
}

bool AutoUpdater::DownloadFileWithResume(
    std::string_view url,
    const std::filesystem::path &outputPath,
    std::function<void(int, size_t, size_t)> progressCallback
) {
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
        {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"}
    };

    HttpClient::DownloadControl control {
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

            const int percentage = total > 0 ? static_cast<int>((downloaded * 100) / total) : 0;

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
                                 std::chrono::steady_clock::now() - currentDownload.startTime
        )
                                 .count();
        const size_t bytesPerSecond
            = elapsed > 0 ? (result.bytesDownloaded - startOffset) / static_cast<size_t>(elapsed) : 0;

        ThreadTask::RunOnMain([progressCallback, bytesPerSecond, total = result.totalBytes]() {
            progressCallback(100, bytesPerSecond, total);
        });
    }

    LOG_INFO("Download complete: {} ({} bytes)", outputPath.string(), result.bytesDownloaded);
    return true;
}

void AutoUpdater::InstallUpdate(const std::filesystem::path &updatePath) {
    LOG_INFO("Installing update from: {}", updatePath.string());

#ifdef __APPLE__
    const auto currentPath = GetAppBundlePath();
    auto backupPath = AltMan::Paths::Backups() / std::format("AltMan_v{}_backup.app", APP_VERSION);
#else
    const auto currentPath = GetCurrentExecutablePath();
    auto backupPath = AltMan::Paths::Backups() / std::format("AltMan_v{}_backup.exe", APP_VERSION);
#endif

    config.backupPath = backupPath;
    config.Save();

    CreateUpdateScript(updatePath.string(), currentPath.string(), backupPath.string());
    LaunchUpdateScript();

    std::exit(0);
}

void AutoUpdater::RollbackToPreviousVersion() {
    if (config.backupPath.empty() || !std::filesystem::exists(config.backupPath)) {
        LOG_ERROR("No backup available for rollback");

        ThreadTask::RunOnMain([]() {
            UpdateNotification::Show("Rollback Failed", "No backup found. Cannot rollback.", 5.0f);
        });
        return;
    }

    ThreadTask::fireAndForget([]() {
#ifdef __APPLE__
        const auto currentPath = GetAppBundlePath();
        const auto tempBackup = std::filesystem::temp_directory_path() / "altman_rollback_tmp.app";
#else
        const auto currentPath = GetCurrentExecutablePath();
        auto tempBackup = std::filesystem::path(currentPath).concat(".rollback_tmp");
#endif

        if (std::filesystem::exists(tempBackup)) {
            std::filesystem::remove_all(tempBackup);
        }

        LOG_INFO("Rolling back from {} to {}", currentPath.string(), tempBackup.string());

        CreateUpdateScript(config.backupPath.string(), currentPath.string(), tempBackup.string());

        ThreadTask::RunOnMain([]() {
            ModalPopup::AddYesNo("Rolling back to previous version. Restart now?", []() {
                LaunchUpdateScript();
                std::exit(0);
            });
        });
    });
}

void AutoUpdater::CleanupOldBackups(int keepCount) {
    const auto backupDir = AltMan::Paths::Backups();

    if (!std::filesystem::exists(backupDir)) {
        return;
    }

    std::vector<std::filesystem::path> backups;

    for (const auto &entry: std::filesystem::directory_iterator(backupDir)) {
        const auto filename = entry.path().filename().string();
        if (filename.find("AltMan") != std::string::npos) {
            backups.push_back(entry.path());
        }
    }

    if (backups.size() <= static_cast<size_t>(keepCount)) {
        return;
    }

    std::ranges::sort(backups, [](const std::filesystem::path &a, const std::filesystem::path &b) {
        return std::filesystem::last_write_time(a) > std::filesystem::last_write_time(b);
    });

    for (size_t i = static_cast<size_t>(keepCount); i < backups.size(); ++i) {
        std::error_code ec;
        std::filesystem::remove_all(backups[i], ec);
        if (!ec) {
            LOG_INFO("Removed old backup: {}", backups[i].string());
        }
    }
}

#ifdef __APPLE__
std::string GetApplicationNameFromBundleId() {
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    if (!mainBundle) {
        return "";
    }

    auto nameRef = static_cast<CFStringRef>(CFBundleGetValueForInfoDictionaryKey(mainBundle, kCFBundleNameKey));
    if (!nameRef) {
        return "";
    }

    std::array<char, 256> buffer {};
    if (!CFStringGetCString(nameRef, buffer.data(), buffer.size(), kCFStringEncodingUTF8)) {
        return "";
    }
    return std::string(buffer.data());
}
#endif
