#pragma once
#include <string>
#include <vector>
#include <chrono>
#include <format>
#include <print>
#include <nlohmann/json.hpp>
#include "network/http.hpp"
#include "core/logging.hpp"
#include "main_thread.h"
#include "ui/confirm.h"
#include "threading.h"
#include "../../version.h"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <thread>
#include <span>
#include <expected>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
    #ifdef __APPLE__
        #include <mach-o/dyld.h>
    #endif
#endif

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

enum class NotificationPosition : uint8_t {
    TopRight,
    TopLeft,
    BottomRight,
    BottomLeft
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

class UpdateNotification {
public:
    struct Notification {
        std::string title;
        std::string message;
        NotificationPosition position{NotificationPosition::TopRight};
        float lifetime;
        float elapsed{0.0f};
        bool canDismiss{true};
        std::function<void()> onClick;

        Notification(std::string_view t, std::string_view m, float life = 5.0f)
            : title(t), message(m), lifetime(life) {}
    };

private:
    inline static std::vector<Notification> activeNotifications;

public:
    static void Show(std::string_view title, std::string_view message,
                    float lifetime = 5.0f, std::function<void()> onClick = nullptr) {
        Notification notif(title, message, lifetime);
        notif.onClick = std::move(onClick);
        activeNotifications.push_back(std::move(notif));
    }

    static void ShowPersistent(std::string_view title, std::string_view message,
                              std::function<void()> onClick = nullptr) {
        Notification notif(title, message, -1.0f);
        notif.onClick = std::move(onClick);
        notif.canDismiss = false;
        activeNotifications.push_back(std::move(notif));
    }

    static void Update(float deltaTime) noexcept {
        std::erase_if(activeNotifications, [deltaTime](Notification& notif) {
            if (notif.lifetime > 0.0f) {
                notif.elapsed += deltaTime;
                return notif.elapsed >= notif.lifetime;
            }
            return false;
        });
    }

    static void Render() {
        if (activeNotifications.empty()) {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        float yOffset = 20.0f;

        constexpr float WINDOW_WIDTH = 300.0f;
        constexpr float BUTTON_SIZE = 20.0f;
        constexpr float WINDOW_PADDING = 12.0f;
        constexpr float ROUNDING = 8.0f;
        constexpr float SPACING_INCREMENT = 130.0f;

        for (auto& notif : activeNotifications) {
            const ImVec2 windowPos = [&]() {
                switch (notif.position) {
                    case NotificationPosition::TopRight:
                        return ImVec2(io.DisplaySize.x - WINDOW_WIDTH - 20.0f, yOffset);
                    case NotificationPosition::TopLeft:
                        return ImVec2(20.0f, yOffset);
                    case NotificationPosition::BottomRight:
                        return ImVec2(io.DisplaySize.x - WINDOW_WIDTH - 20.0f, io.DisplaySize.y - 150.0f);
                    case NotificationPosition::BottomLeft:
                    default:
                        return ImVec2(20.0f, io.DisplaySize.y - 150.0f);
                }
            }();

            ImGui::SetNextWindowPos(windowPos);
            ImGui::SetNextWindowSize(ImVec2(WINDOW_WIDTH, 0));

            constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                              ImGuiWindowFlags_NoMove |
                                              ImGuiWindowFlags_NoSavedSettings |
                                              ImGuiWindowFlags_AlwaysAutoResize;

            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ROUNDING);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(WINDOW_PADDING, WINDOW_PADDING));

            const auto windowLabel = std::format("##Notification{}", static_cast<const void*>(&notif));
            if (ImGui::Begin(windowLabel.c_str(), nullptr, flags)) {
                if (notif.canDismiss) {
                    ImGui::SameLine(ImGui::GetWindowWidth() - BUTTON_SIZE - 10.0f);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 0.5f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.9f, 0.1f, 0.1f, 0.7f));

                    if (ImGui::Button("X", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                        notif.elapsed = notif.lifetime;
                    }
                    ImGui::PopStyleColor(3);
                }

                ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
                ImGui::TextWrapped("%s", notif.title.c_str());
                ImGui::PopFont();

                ImGui::Spacing();
                ImGui::TextWrapped("%s", notif.message.c_str());

                if (notif.onClick) {
                    ImGui::Spacing();
                    if (ImGui::Button("View", ImVec2(-1, 0))) {
                        notif.onClick();
                    }
                }

                if (notif.lifetime > 0.0f) {
                    const float progress = notif.elapsed / notif.lifetime;
                    ImGui::ProgressBar(progress, ImVec2(-1, 2));
                }
            }
            ImGui::End();
            ImGui::PopStyleVar(2);

            yOffset += SPACING_INCREMENT;
        }
    }

    static void Clear() noexcept {
        activeNotifications.clear();
    }
};

class UpdaterConfig {
private:
    [[nodiscard]] static std::filesystem::path GetConfigPath() {
        std::filesystem::path path;

#ifdef _WIN32
        if (const char* appdata = std::getenv("APPDATA")) {
            path = std::filesystem::path(appdata) / "AltMan" / "updater.json";
        }
#else
        if (const char* home = std::getenv("HOME")) {
#ifdef __APPLE__
            path = std::filesystem::path(home) / "Library" / "Application Support" / "Altman" / "storage" / "updater.json";
#else
            path = std::filesystem::path(home) / ".config" / "AltMan" / "updater.json";
#endif
        }
#endif

        std::filesystem::create_directories(path.parent_path());
        return path;
    }

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

    void Save() const {
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

    void Load() {
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
};

class AutoUpdater {
private:
    inline static UpdaterConfig config;
    inline static std::filesystem::path pendingUpdatePath;
    inline static DownloadState currentDownload;

    [[nodiscard]] static constexpr std::string_view GetChannelName(UpdateChannel channel) noexcept {
        switch (channel) {
            case UpdateChannel::Beta: return "beta";
            case UpdateChannel::Dev: return "dev";
            default: return "stable";
        }
    }

    [[nodiscard]] static std::string GetPlatformAssetName(UpdateChannel channel) {
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

    [[nodiscard]] static std::string GetDeltaAssetName(std::string_view fromVersion, std::string_view toVersion) {
#ifdef _WIN32
        constexpr std::string_view extension = ".patch";
#else
        constexpr std::string_view extension = ".bsdiff";
#endif
        return std::format("AltMan-Delta-{}-to-{}{}", fromVersion, toVersion, extension);
    }

    [[nodiscard]] static std::filesystem::path GetBackupPath() {
        std::filesystem::path path;

#ifdef _WIN32
        if (const char* appdata = std::getenv("APPDATA")) {
            path = std::filesystem::path(appdata) / "AltMan" / "backups";
        }
#else
        if (const char* home = std::getenv("HOME")) {
#ifdef __APPLE__
            path = std::filesystem::path(home) / "Library" / "Application Support" / "Altman" / "backups";
#else
            path = std::filesystem::path(home) / ".local" / "share" / "AltMan" / "backups";
#endif
        }
#endif

        std::filesystem::create_directories(path);
        return path;
    }

    [[nodiscard]] static std::filesystem::path GetUpdateScriptPath() {
#ifdef _WIN32
        return std::filesystem::temp_directory_path() / "update_altman.bat";
#else
        return std::filesystem::temp_directory_path() / "update_altman.sh";
#endif
    }

    static void CreateUpdateScript(const std::string& newExecutable, const std::string& currentExecutable, const std::string& backupPath) {
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

    static void LaunchUpdateScript() {
        const auto scriptPath = GetUpdateScriptPath();

#ifdef _WIN32
        ShellExecuteW(nullptr, L"open", scriptPath.c_str(), nullptr, nullptr, SW_HIDE);
#else
        const auto command = std::format("\"{}\" &", scriptPath.string());
        std::system(command.c_str());
#endif
    }

    [[nodiscard]] static std::string FormatBytes(size_t bytes) noexcept {
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

    [[nodiscard]] static std::string FormatSpeed(size_t bytesPerSecond) noexcept {
        return std::format("{}/s", FormatBytes(bytesPerSecond));
    }

    static bool ApplyDeltaPatch(const std::filesystem::path& oldFile, const std::filesystem::path& patchFile, const std::filesystem::path& newFile) {
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

    [[nodiscard]] static UpdateInfo ParseReleaseInfo(const nlohmann::json& release, UpdateChannel channel) {
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

    [[nodiscard]] static std::string GetReleaseEndpoint(UpdateChannel channel) {
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

    [[nodiscard]] static bool MatchesChannel(const nlohmann::json& release, UpdateChannel channel) {
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

public:
    static void Initialize() {
        config.Load();

        if (config.autoCheck) {
            StartBackgroundChecker();
        }
    }

    static void SetUpdateChannel(UpdateChannel channel) {
        config.channel = channel;
        config.Save();
        LOG_INFO(std::format("Update channel set to: {}", GetChannelName(channel)));
    }

    [[nodiscard]] static UpdateChannel GetUpdateChannel() noexcept {
        return config.channel;
    }

    static void SetAutoUpdate(bool autoCheck, bool autoDownload, bool autoInstall) {
        config.autoCheck = autoCheck;
        config.autoDownload = autoDownload;
        config.autoInstall = autoInstall;
        config.Save();

        if (autoCheck) {
            StartBackgroundChecker();
        }
    }

    static void SetBandwidthLimit(size_t bytesPerSecond) {
        config.bandwidthLimit = bytesPerSecond;
        config.Save();
        LOG_INFO(std::format("Bandwidth limit set to: {}", FormatSpeed(bytesPerSecond)));
    }

    static void SetShowNotifications(bool show) noexcept {
        config.showNotifications = show;
        config.Save();
    }

    static void PauseDownload() noexcept {
        currentDownload.isPaused = true;
    }

    static void ResumeDownload() noexcept {
        currentDownload.isPaused = false;
    }

    static void CancelDownload() noexcept {
        currentDownload.shouldCancel = true;
    }

    [[nodiscard]] static const DownloadState& GetDownloadState() noexcept {
        return currentDownload;
    }

    static void StartBackgroundChecker() {
        Threading::newThread([]() {
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

    static void CheckForUpdates(bool silent = false) {
        Threading::newThread([silent]() {
            const auto endpoint = GetReleaseEndpoint(config.channel);
            const auto resp = HttpClient::get(endpoint, {
                {"User-Agent", "AltMan"},
                {"Accept", "application/vnd.github+json"}
            });

            if (resp.status_code != 200) {
                LOG_INFO(std::format("Failed to check for updates: HTTP {}", resp.status_code));

                if (!silent) {
                    const auto showError = []() {
                        if (config.showNotifications) {
                            UpdateNotification::Show("Update Check Failed",
                                "Failed to check for updates. Please try again later.");
                        } else {
                            ConfirmPopup::AddInfo("Failed to check for updates. Please try again later.");
                        }
                    };
                    MainThread::Post(showError);
                }
                return;
            }

            const nlohmann::json releases = HttpClient::decode(resp);
            UpdateInfo updateInfo;
            bool foundUpdate = false;

            if (config.channel == UpdateChannel::Stable && releases.is_object()) {
                updateInfo = ParseReleaseInfo(releases, config.channel);
                foundUpdate = (!updateInfo.version.empty() && updateInfo.version != APP_VERSION);
            } else if (releases.is_array()) {
                for (const auto& release : releases) {
                    if (MatchesChannel(release, config.channel)) {
                        updateInfo = ParseReleaseInfo(release, config.channel);
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
                            ConfirmPopup::AddInfo("You're using the latest version!");
                        }
                    };
                    MainThread::Post(showUpToDate);
                }
                return;
            }

            MainThread::Post([updateInfo, silent]() {
                HandleUpdateAvailable(updateInfo, silent);
            });
        });
    }

    static void HandleUpdateAvailable(const UpdateInfo& info, bool silent) {
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
                ConfirmPopup::AddYesNo("Download and install update?", [info]() {
                    DownloadAndInstallUpdate(info);
                });
            });
        } else {
            if (config.autoDownload && !silent) {
                msg = std::format("{}\n\nDownload automatically?", msg);
                ConfirmPopup::AddOk(msg, [info]() {
                    DownloadAndInstallUpdate(info);
                });
            } else if (config.autoDownload && config.autoInstall && silent) {
                DownloadAndInstallUpdate(info, true);
            } else if (!silent) {
                ConfirmPopup::AddOk(msg, [info]() {
                    DownloadAndInstallUpdate(info);
                });
            }
        }
    }

    static void DownloadAndInstallUpdate(const UpdateInfo& info, bool autoInstall = false) {
        Threading::newThread([info, autoInstall]() {
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
            MainThread::Post(showDownloadStart);

            if (useDelta) {
                const auto patchPath = std::filesystem::temp_directory_path() / "altman_update.patch";

                const auto progressCallback = [](int percentage, size_t speed, size_t total) {
                    LOG_INFO(std::format("Download progress: {}% - {} - {}",
                        percentage, FormatSpeed(speed), FormatBytes(total)));
                };

                success = DownloadFileWithResume(info.deltaUrl, patchPath, progressCallback);

                if (success) {
                    const auto showPatchApply = []() {
                        if (config.showNotifications) {
                            UpdateNotification::Show("Applying Patch",
                                "Applying delta patch...", 3.0f);
                        }
                    };
                    MainThread::Post(showPatchApply);

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
                    LOG_INFO(std::format("Download progress: {}% - {} - {}",
                        percentage, FormatSpeed(speed), FormatBytes(total)));
                };

                success = DownloadFileWithResume(info.downloadUrl, tempPath, progressCallback);
            }

            if (!success) {
                const auto showError = []() {
                    if (config.showNotifications) {
                        UpdateNotification::Show("Download Failed",
                            "Download failed. Please try again later.", 5.0f);
                    } else {
                        ConfirmPopup::AddInfo("Download failed. Please try again later.");
                    }
                };
                MainThread::Post(showError);
                return;
            }

            const auto showComplete = []() {
                if (config.showNotifications) {
                    UpdateNotification::Show("Download Complete",
                        "Update downloaded successfully!", 3.0f);
                }
            };
            MainThread::Post(showComplete);

            pendingUpdatePath = tempPath;
            config.lastInstalledVersion = info.version;

            if (autoInstall || config.autoInstall) {
                MainThread::Post([tempPath]() {
                    InstallUpdate(tempPath);
                });
            } else {
                if (config.showNotifications) {
                    MainThread::Post([tempPath]() {
                        UpdateNotification::ShowPersistent("Update Ready",
                            "Click to install and restart", [tempPath]() {
                                InstallUpdate(tempPath);
                            });
                    });
                } else {
                    MainThread::Post([tempPath]() {
                        ConfirmPopup::AddYesNo("Update ready! Restart now to install?",
                            [tempPath]() {
                                InstallUpdate(tempPath);
                            });
                    });
                }
            }
        });
    }

	static bool DownloadFileWithResume(std::string_view url, const std::filesystem::path& outputPath,
                                      std::function<void(int, size_t, size_t)> progressCallback) {
	    LOG_INFO(std::format("Downloading: {}", url));

    	currentDownload.url = url;
    	currentDownload.outputPath = outputPath.string();
    	currentDownload.isPaused = false;
    	currentDownload.shouldCancel = false;

    	size_t startOffset = 0;
    	std::ios::openmode mode = std::ios::binary;

    	if (config.resumeFilePath == outputPath && config.resumeOffset > 0) {
    		if (std::filesystem::exists(outputPath)) {
    			startOffset = config.resumeOffset;
    			mode |= std::ios::app;
    			LOG_INFO(std::format("Resuming download from byte {}", startOffset));
    		}
    	}

    	std::vector<std::pair<std::string, std::string>> headers = {
    		{"User-Agent", "AltMan"}
    	};

    	if (startOffset > 0) {
    		headers.emplace_back("Range", std::format("bytes={}-", startOffset));
    	}

    	auto resp = HttpClient::get(std::string(url), headers);

    	if (resp.status_code != 200 && resp.status_code != 206) {
    		LOG_INFO(std::format("Download failed: HTTP {}", resp.status_code));
    		return false;
    	}

    	std::ofstream file(outputPath, mode);
    	if (!file.is_open()) {
    		LOG_INFO(std::format("Failed to open file: {}", outputPath.string()));
    		return false;
    	}

    	const size_t contentLength = resp.text.size();
    	currentDownload.totalBytes = contentLength + startOffset;
    	currentDownload.downloadedBytes = startOffset;
    	currentDownload.startTime = std::chrono::steady_clock::now();
    	currentDownload.lastUpdateTime = currentDownload.startTime;

    	const size_t chunkSize = config.bandwidthLimit > 0
			? std::min(1_MB, config.bandwidthLimit)
			: 1_MB;

    	if (progressCallback) {
    		const int percentage = startOffset > 0
				? static_cast<int>((startOffset * 100) / currentDownload.totalBytes)
				: 0;

    		MainThread::Post([progressCallback, percentage, total = currentDownload.totalBytes]() {
				progressCallback(percentage, 0, total);
			});
    	}

    	for (size_t i = 0; i < contentLength; i += chunkSize) {
    		if (currentDownload.shouldCancel) {
    			file.close();
    			config.resumeFilePath = outputPath;
    			config.resumeOffset = currentDownload.downloadedBytes;
    			config.Save();
    			LOG_INFO("Download cancelled, progress saved for resume");
    			return false;
    		}

    		while (currentDownload.isPaused) {
    			std::this_thread::sleep_for(std::chrono::milliseconds(100));
    		}

    		const size_t writeSize = std::min(chunkSize, contentLength - i);
    		file.write(resp.text.data() + i, static_cast<std::streamsize>(writeSize));
    		currentDownload.downloadedBytes += writeSize;

    		if (config.bandwidthLimit > 0) {
    			const auto now = std::chrono::steady_clock::now();
    			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
					now - currentDownload.lastUpdateTime).count();

    			const auto expectedTime = static_cast<long long>((writeSize * 1000) / config.bandwidthLimit);
    			if (elapsed < expectedTime) {
    				std::this_thread::sleep_for(std::chrono::milliseconds(expectedTime - elapsed));
    			}
    			currentDownload.lastUpdateTime = std::chrono::steady_clock::now();
    		}

    		if (progressCallback) {
    			const int percentage = static_cast<int>(
					(currentDownload.downloadedBytes * 100) / currentDownload.totalBytes);

    			const auto now = std::chrono::steady_clock::now();
    			const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
					now - currentDownload.startTime).count();

    			const size_t bytesPerSecond = elapsed > 0
					? (currentDownload.downloadedBytes - startOffset) / static_cast<size_t>(elapsed)
					: 0;

    			MainThread::Post([progressCallback, percentage, bytesPerSecond, total = currentDownload.totalBytes]() {
					progressCallback(percentage, bytesPerSecond, total);
				});
    		}
    	}

    	file.close();
    	currentDownload.isComplete = true;

    	if (progressCallback) {
    		const auto now = std::chrono::steady_clock::now();
    		const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
				now - currentDownload.startTime).count();

    		const size_t bytesPerSecond = elapsed > 0
				? (currentDownload.downloadedBytes - startOffset) / static_cast<size_t>(elapsed)
				: 0;

    		MainThread::Post([progressCallback, bytesPerSecond, total = currentDownload.totalBytes]() {
				progressCallback(100, bytesPerSecond, total);
			});
    	}

    	config.resumeFilePath.clear();
    	config.resumeOffset = 0;
    	config.Save();

    	LOG_INFO(std::format("Download complete: {}", outputPath.string()));
    	return true;
    }

    static void InstallUpdate(const std::filesystem::path& updatePath) {
        const auto currentExe = GetCurrentExecutablePath();
        auto backupPath = GetBackupPath() / std::format("altman_v{}_backup", APP_VERSION);

#ifdef _WIN32
        backupPath.replace_extension(".exe");
#endif

        config.backupPath = backupPath;
        config.Save();

        CreateUpdateScript(updatePath, currentExe, backupPath);
        LaunchUpdateScript();

        std::exit(0);
    }

    static void RollbackToPreviousVersion() {
        if (config.backupPath.empty() || !std::filesystem::exists(config.backupPath)) {
            LOG_INFO("No backup available for rollback");

            const auto showError = []() {
                if (config.showNotifications) {
                    UpdateNotification::Show("Rollback Failed",
                        "No backup found. Cannot rollback.", 5.0f);
                } else {
                    ConfirmPopup::AddInfo("No backup found. Cannot rollback.");
                }
            };
            MainThread::Post(showError);
            return;
        }

        Threading::newThread([]() {
            const auto currentExe = GetCurrentExecutablePath();
            const auto tempBackup = std::filesystem::path(currentExe).concat(".rollback_tmp");

            std::filesystem::copy_file(currentExe, tempBackup, std::filesystem::copy_options::overwrite_existing);
            CreateUpdateScript(config.backupPath, currentExe, tempBackup);

            const auto confirmRollback = []() {
                ConfirmPopup::AddYesNo("Rolling back to previous version. Restart now?",
                    []() {
                        LaunchUpdateScript();
                        std::exit(0);
                    });
            };
            MainThread::Post(confirmRollback);
        });
    }

    static void CleanupOldBackups(int keepCount = 3) {
        const auto backupDir = GetBackupPath();
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
            LOG_INFO(std::format("Removed old backup: {}", backups[i].string()));
        }
    }

    [[nodiscard]] static std::filesystem::path GetCurrentExecutablePath() {
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
};

