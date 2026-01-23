#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

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
        std::string changelog;
        size_t fullSize {0};
        std::string sha256;
        UpdateChannel channel {UpdateChannel::Stable};
        bool isCritical {false};

        // Windows: single delta
        std::string deltaUrl;
        size_t deltaSize {0};
        std::string deltaSha256;

        // macOS: dual deltas for universal binary
        std::string deltaUrl_arm64;
        std::string deltaUrl_x86_64;
        size_t deltaSize_arm64 {0};
        size_t deltaSize_x86_64 {0};

        [[nodiscard]] bool hasDelta() const noexcept {
#ifdef __APPLE__
            // macOS needs both patches for universal binary
            return !deltaUrl_arm64.empty() && !deltaUrl_x86_64.empty();
#else
            return !deltaUrl.empty();
#endif
        }

        [[nodiscard]] size_t totalDeltaSize() const noexcept {
#ifdef __APPLE__
            return deltaSize_arm64 + deltaSize_x86_64;
#else
            return deltaSize;
#endif
        }
};

struct DownloadState {
        std::string url;
        std::string outputPath;
        std::atomic<size_t> totalBytes {0};
        std::atomic<size_t> downloadedBytes {0};
        std::chrono::steady_clock::time_point startTime;
        std::chrono::steady_clock::time_point lastUpdateTime;
        std::atomic<bool> isPaused {false};
        std::atomic<bool> isComplete {false};
        std::atomic<bool> shouldCancel {false};

        DownloadState() = default;
        DownloadState(const DownloadState &) = delete;
        DownloadState &operator=(const DownloadState &) = delete;

        DownloadState(DownloadState &&other) noexcept :
            url(std::move(other.url)),
            outputPath(std::move(other.outputPath)),
            totalBytes(other.totalBytes.load()),
            downloadedBytes(other.downloadedBytes.load()),
            startTime(other.startTime),
            lastUpdateTime(other.lastUpdateTime),
            isPaused(other.isPaused.load()),
            isComplete(other.isComplete.load()),
            shouldCancel(other.shouldCancel.load()) {
        }

        DownloadState &operator=(DownloadState &&other) noexcept {
            if (this != &other) {
                url = std::move(other.url);
                outputPath = std::move(other.outputPath);
                totalBytes.store(other.totalBytes.load());
                downloadedBytes.store(other.downloadedBytes.load());
                startTime = other.startTime;
                lastUpdateTime = other.lastUpdateTime;
                isPaused.store(other.isPaused.load());
                isComplete.store(other.isComplete.load());
                shouldCancel.store(other.shouldCancel.load());
            }
            return *this;
        }

        void reset() {
            url.clear();
            outputPath.clear();
            totalBytes.store(0);
            downloadedBytes.store(0);
            startTime = {};
            lastUpdateTime = {};
            isPaused.store(false);
            isComplete.store(false);
            shouldCancel.store(false);
        }
};

class UpdaterConfig {
    public:
        UpdateChannel channel {UpdateChannel::Stable};
        bool autoCheck {true};
        bool autoDownload {false};
        bool autoInstall {false};
        size_t bandwidthLimit {0};
        std::chrono::system_clock::time_point lastCheck;
        std::string lastInstalledVersion;
        std::filesystem::path backupPath;
        std::filesystem::path resumeFilePath;
        size_t resumeOffset {0};

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

        static void PauseDownload() noexcept;
        static void ResumeDownload() noexcept;
        static void CancelDownload() noexcept;
        [[nodiscard]] static const DownloadState &GetDownloadState() noexcept;

        static void StartBackgroundChecker();
        static void CheckForUpdates(bool silent = false);
        static void HandleUpdateAvailable(const UpdateInfo &info, bool silent);
        static void DownloadAndInstallUpdate(const UpdateInfo &info, bool autoInstall = false);

        static bool DownloadFileWithResume(
            std::string_view url,
            const std::filesystem::path &outputPath,
            std::function<void(int, size_t, size_t)> progressCallback
        );

        static void InstallUpdate(const std::filesystem::path &updatePath);
        static void RollbackToPreviousVersion();
        static void CleanupOldBackups(int keepCount = 3);

        [[nodiscard]] static std::filesystem::path GetCurrentExecutablePath();
        [[nodiscard]] static std::filesystem::path GetAppBundlePath();

    private:
        [[nodiscard]] static constexpr std::string_view GetChannelName(UpdateChannel channel) noexcept;
        [[nodiscard]] static std::string GetPlatformAssetName(UpdateChannel channel);
        [[nodiscard]] static std::string GetDeltaAssetName(std::string_view fromVersion, std::string_view toVersion);
        [[nodiscard]] static std::filesystem::path GetUpdateScriptPath();

        static void
        CreateUpdateScript(const std::string &newPath, const std::string &currentPath, const std::string &backupPath);
        static void LaunchUpdateScript();

        [[nodiscard]] static std::string FormatBytes(size_t bytes) noexcept;
        [[nodiscard]] static std::string FormatSpeed(size_t bytesPerSecond) noexcept;

        static bool ApplyDeltaPatch(
            const std::filesystem::path &oldFile,
            const std::filesystem::path &patchFile,
            const std::filesystem::path &newFile
        );

#ifdef __APPLE__
        static bool ExtractZipToPath(const std::filesystem::path &zipPath, const std::filesystem::path &destPath);

        [[nodiscard]] static bool IsUniversalBinary(const std::filesystem::path &binaryPath);
        static bool ExtractSlice(
            const std::filesystem::path &binaryPath,
            const std::string &arch,
            const std::filesystem::path &outputPath
        );
        static bool CreateUniversalBinary(
            const std::filesystem::path &arm64Path,
            const std::filesystem::path &x86_64Path,
            const std::filesystem::path &outputPath
        );
        static bool ApplyUniversalDeltaUpdate(
            const std::filesystem::path &arm64PatchPath,
            const std::filesystem::path &x86_64PatchPath,
            const std::filesystem::path &outputAppPath
        );
#endif

#ifdef _WIN32
        [[nodiscard]] static std::string GetHardwareArchitecture();
        [[nodiscard]] static bool IsRunningUnderEmulation();
#endif

        [[nodiscard]] static UpdateInfo ParseReleaseInfo(const nlohmann::json &release, UpdateChannel channel);
        [[nodiscard]] static std::string GetReleaseEndpoint(UpdateChannel channel);
        [[nodiscard]] static bool MatchesChannel(const nlohmann::json &release, UpdateChannel channel);
};
