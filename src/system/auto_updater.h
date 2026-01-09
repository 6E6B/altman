#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <filesystem>
#include <functional>
#include <atomic>
#include <cstdint>
#include <nlohmann/json.hpp>

[[nodiscard]] constexpr size_t operator""_KB(unsigned long long value) noexcept {
    return value * 1024ULL;
}

[[nodiscard]] constexpr size_t operator""_MB(unsigned long long value) noexcept {
    return value * 1024ULL * 1024ULL;
}

[[nodiscard]] constexpr size_t operator""_GB(unsigned long long value) noexcept {
    return value * 1024ULL * 1024ULL * 1024ULL;
}

enum class UpdateChannel : uint8_t {
    Stable,
    Beta,
    Dev
};

struct UpdateInfo {
    std::string version;
    std::string downloadUrl;
    std::string deltaUrl;
    std::string changelog;
    size_t fullSize{0};
    size_t deltaSize{0};
    std::string sha256;
    UpdateChannel channel{UpdateChannel::Stable};
    bool isCritical{false};
};

struct DownloadState {
    std::string url;
    std::string outputPath;
    size_t totalBytes{0};
    size_t downloadedBytes{0};
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point lastUpdateTime;
    std::atomic<bool> isPaused{false};
    std::atomic<bool> isComplete{false};
    std::atomic<bool> shouldCancel{false};
};

class UpdaterConfig {
public:
    UpdateChannel channel{UpdateChannel::Stable};
    bool autoCheck{true};
    bool autoDownload{false};
    bool autoInstall{false};
    bool showNotifications{true};
    size_t bandwidthLimit{0};
    std::chrono::system_clock::time_point lastCheck;
    std::string lastInstalledVersion;
    std::filesystem::path backupPath;
    std::filesystem::path resumeFilePath;
    size_t resumeOffset{0};

    void Save() const;
    void Load();

private:
    [[nodiscard]] static std::filesystem::path GetConfigPath();
};

class AutoUpdater {
public:
    static void Initialize();
    static void SetUpdateChannel(UpdateChannel channel);
    [[nodiscard]] static UpdateChannel GetUpdateChannel() noexcept;
    static void SetAutoUpdate(bool autoCheck, bool autoDownload, bool autoInstall);
    static void SetBandwidthLimit(size_t bytesPerSecond);
    static void SetShowNotifications(bool show) noexcept;

    static void PauseDownload() noexcept;
    static void ResumeDownload() noexcept;
    static void CancelDownload() noexcept;
    [[nodiscard]] static const DownloadState& GetDownloadState() noexcept;

    static void StartBackgroundChecker();
    static void CheckForUpdates(bool silent = false);
    static void HandleUpdateAvailable(const UpdateInfo& info, bool silent);
    static void DownloadAndInstallUpdate(const UpdateInfo& info, bool autoInstall = false);

    static bool DownloadFileWithResume(std::string_view url, const std::filesystem::path& outputPath,
                                      std::function<void(int, size_t, size_t)> progressCallback);

    static void InstallUpdate(const std::filesystem::path& updatePath);
    static void RollbackToPreviousVersion();
    static void CleanupOldBackups(int keepCount = 3);

    [[nodiscard]] static std::filesystem::path GetCurrentExecutablePath();

private:
    [[nodiscard]] static constexpr std::string_view GetChannelName(UpdateChannel channel) noexcept;
    [[nodiscard]] static std::string GetPlatformAssetName(UpdateChannel channel);
    [[nodiscard]] static std::string GetDeltaAssetName(std::string_view fromVersion, std::string_view toVersion);
    [[nodiscard]] static std::filesystem::path GetBackupPath();
    [[nodiscard]] static std::filesystem::path GetUpdateScriptPath();

    static void CreateUpdateScript(const std::string& newExecutable, const std::string& currentExecutable, const std::string& backupPath);
    static void LaunchUpdateScript();

    [[nodiscard]] static std::string FormatBytes(size_t bytes) noexcept;
    [[nodiscard]] static std::string FormatSpeed(size_t bytesPerSecond) noexcept;

    static bool ApplyDeltaPatch(const std::filesystem::path& oldFile, const std::filesystem::path& patchFile, const std::filesystem::path& newFile);

    [[nodiscard]] static UpdateInfo ParseReleaseInfo(const nlohmann::json& release, UpdateChannel channel);
    [[nodiscard]] static std::string GetReleaseEndpoint(UpdateChannel channel);
    [[nodiscard]] static bool MatchesChannel(const nlohmann::json& release, UpdateChannel channel);
};