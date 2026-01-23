#include "client_update_checker_macos.h"

#include <format>
#include <fstream>

#include <nlohmann/json.hpp>

#include "auto_updater.h"
#include "components/data.h"
#include "console/console.h"
#include "network/client_manager_macos.h"
#include "system/multi_instance.h"
#include "ui/widgets/notifications.h"
#include "utils/paths.h"
#include "utils/thread_task.h"

namespace ClientUpdateChecker {

    namespace {
        std::mutex versionsMutex;
        std::unordered_map<std::string, ClientVersionInfo> clientVersions;
        std::unordered_set<std::string> clientsCurrentlyUpdating;

        std::atomic<bool> isRunning {false};
        std::atomic<bool> shouldStop {false};
        std::thread checkerThread;
        std::condition_variable shutdownCV;
        std::mutex shutdownMutex;

        std::once_flag configPathInitFlag;
        std::filesystem::path configPath;

        void InitConfigPath() {
            std::call_once(configPathInitFlag, []() {
                configPath = UpdateChecker::GetConfigPath();
            });
        }
    } // namespace

    std::filesystem::path UpdateChecker::GetConfigPath() {
        return AltMan::Paths::Config("client_versions.json");
    }

    void UpdateChecker::SaveVersionInfoLocked() {
        InitConfigPath();

        std::error_code ec;
        std::filesystem::create_directories(configPath.parent_path(), ec);
        if (ec) {
            LOG_ERROR("Failed to create config directory: {}", ec.message());
            return;
        }

        nlohmann::json j = nlohmann::json::object();

        for (const auto &[clientName, info]: clientVersions) {
            j[clientName] = {
                {"installedVersion", info.installedVersion                                 },
                {"latestVersion",    info.latestVersion                                    },
                {"lastChecked",      std::chrono::system_clock::to_time_t(info.lastChecked)},
                {"updateAvailable",  info.updateAvailable                                  }
            };
        }

        std::ofstream file(configPath);
        if (file.is_open()) {
            file << j.dump(2);
            file.close();
        } else {
            LOG_ERROR("Failed to open config file for writing: {}", configPath.string());
        }
    }

    void UpdateChecker::SaveVersionInfo() {
        std::lock_guard lock(versionsMutex);
        SaveVersionInfoLocked();
    }

    void UpdateChecker::LoadVersionInfo() {
        InitConfigPath();

        if (!std::filesystem::exists(configPath)) {
            return;
        }

        std::ifstream file(configPath);
        if (!file.is_open()) {
            LOG_ERROR("Failed to open config file for reading: {}", configPath.string());
            return;
        }

        try {
            nlohmann::json j;
            file >> j;
            file.close();

            std::lock_guard lock(versionsMutex);

            for (auto &[clientName, data]: j.items()) {
                ClientVersionInfo info;
                info.installedVersion = data.value("installedVersion", "");
                info.latestVersion = data.value("latestVersion", "");
                info.updateAvailable = data.value("updateAvailable", false);

                auto lastCheckedTime = data.value("lastChecked", 0LL);
                info.lastChecked = std::chrono::system_clock::from_time_t(lastCheckedTime);

                clientVersions[clientName] = info;
            }
        } catch (const std::exception &e) {
            LOG_ERROR("Failed to load client versions: {}", e.what());
        }
    }

    bool UpdateChecker::TryBeginClientUpdate(const std::string &clientName) {
        std::lock_guard lock(versionsMutex);
        if (clientsCurrentlyUpdating.contains(clientName)) {
            return false;
        }
        clientsCurrentlyUpdating.insert(clientName);
        return true;
    }

    void UpdateChecker::EndClientUpdate(const std::string &clientName) {
        std::lock_guard lock(versionsMutex);
        clientsCurrentlyUpdating.erase(clientName);
    }

    bool UpdateChecker::IsClientUpdating(const std::string &clientName) {
        std::lock_guard lock(versionsMutex);
        return clientsCurrentlyUpdating.contains(clientName);
    }

    void UpdateChecker::CheckClientForUpdate(const std::string &clientName) {
        if (!MultiInstance::isBaseClientInstalled(clientName)) {
            return;
        }

        if (IsClientUpdating(clientName)) {
            LOG_INFO("Skipping update check for {} - update already in progress", clientName);
            return;
        }

        LOG_INFO("Checking for updates: {}", clientName);

        std::string latestVersion = GetClientVersion(clientName);

        if (latestVersion.empty()) {
            LOG_WARN("Failed to fetch latest version for {}", clientName);
            return;
        }

        ClientVersionInfo infoCopy;
        bool shouldUpdate = false;

        {
            std::lock_guard lock(versionsMutex);

            auto &info = clientVersions[clientName];
            info.latestVersion = latestVersion;
            info.lastChecked = std::chrono::system_clock::now();

            if (info.installedVersion.empty()) {
                info.installedVersion = latestVersion;
                info.updateAvailable = false;
                LOG_INFO("{} version initialized: {}", clientName, latestVersion);
                SaveVersionInfoLocked();
                return;
            }

            if (info.installedVersion != latestVersion) {
                info.updateAvailable = true;
                LOG_INFO("{} update available: {} -> {}", clientName, info.installedVersion, latestVersion);
                SaveVersionInfoLocked();
                infoCopy = info;
                shouldUpdate = true;
            } else {
                info.updateAvailable = false;
                LOG_INFO("{} is up to date: {}", clientName, latestVersion);
                SaveVersionInfoLocked();
            }
        }

        if (shouldUpdate) {
            ThreadTask::RunOnMain([clientName, infoCopy]() {
                NotifyAndUpdate(clientName, infoCopy);
            });
        }
    }

    void UpdateChecker::NotifyAndUpdate(const std::string &clientName, const ClientVersionInfo &info) {
        if (!TryBeginClientUpdate(clientName)) {
            LOG_WARN("Cannot update {} - update already in progress", clientName);
            UpdateNotification::Show("Update Skipped", std::format("{} is already being updated", clientName), 3.0f);
            return;
        }

        std::string message
            = std::format("Updating {} from {} to {}...", clientName, info.installedVersion, info.latestVersion);

        UpdateNotification::Show("Client Update", message, 5.0f);

        LOG_INFO("Starting auto-update for {}", clientName);

        std::string clientPath = MultiInstance::getBaseClientPath(clientName);
        if (!clientPath.empty() && std::filesystem::exists(clientPath)) {
            std::error_code ec;
            std::filesystem::remove_all(clientPath, ec);
            if (ec) {
                LOG_ERROR("Failed to remove old {}: {}", clientName, ec.message());
                UpdateNotification::Show(
                    "Update Failed",
                    std::format("Failed to remove old {} client", clientName),
                    5.0f
                );
                EndClientUpdate(clientName);
                return;
            }
        }

        std::string clientNameCopy = clientName;

        auto progressCallback = [clientNameCopy](float progress, const std::string &msg) {
            // LOG_INFO("{} update progress: {:.0f}% - {}", clientNameCopy, progress * 100.0f, msg);
        };

        auto completionCallback = [clientNameCopy](bool success, const std::string &message) {
            if (success) {
                LOG_INFO("{} updated successfully", clientNameCopy);

                {
                    std::lock_guard lock(versionsMutex);
                    auto &info = clientVersions[clientNameCopy];
                    info.installedVersion = info.latestVersion;
                    info.updateAvailable = false;
                    SaveVersionInfoLocked();
                }

                ThreadTask::RunOnMain([clientNameCopy]() {
                    UpdateNotification::Show(
                        "Update Complete",
                        std::format("{} has been updated successfully!", clientNameCopy),
                        5.0f
                    );
                });
            } else {
                LOG_ERROR("{} update failed: {}", clientNameCopy, message);

                ThreadTask::RunOnMain([clientNameCopy, message]() {
                    UpdateNotification::Show("Update Failed", std::format("{}: {}", clientNameCopy, message), 5.0f);
                });
            }

            EndClientUpdate(clientNameCopy);
        };

        ClientManager::InstallClientAsync(clientName, progressCallback, completionCallback);
    }

    void UpdateChecker::CheckerLoop() {
        LOG_INFO("Client update checker started");

        while (!shouldStop.load()) {

            for (const auto &clientName: g_availableClientsNames) {
                if (shouldStop.load()) {
                    break;
                }

                if (clientName != "Default" && !MultiInstance::isBaseClientInstalled(clientName)) {
                    continue;
                }

                bool shouldCheck = false;
                {
                    std::lock_guard lock(versionsMutex);
                    auto it = clientVersions.find(clientName);
                    if (it == clientVersions.end()) {
                        shouldCheck = true;
                    } else {
                        auto now = std::chrono::system_clock::now();
                        auto elapsed
                            = std::chrono::duration_cast<std::chrono::hours>(now - it->second.lastChecked).count();
                        shouldCheck
                            = (elapsed >= 24 || it->second.lastChecked == std::chrono::system_clock::time_point {});
                    }
                }

                if (shouldCheck) {
                    CheckClientForUpdate(clientName);
                }

                {
                    std::unique_lock lock(shutdownMutex);
                    if (shutdownCV.wait_for(lock, std::chrono::seconds(2), []() {
                            return shouldStop.load();
                        })) {
                        break;
                    }
                }
            }

            {
                std::unique_lock lock(shutdownMutex);
                shutdownCV.wait_for(lock, std::chrono::hours(1), []() {
                    return shouldStop.load();
                });
            }
        }

        LOG_INFO("Client update checker stopped");
    }

    void UpdateChecker::Initialize() {
        if (isRunning.load()) {
            LOG_WARN("Client update checker already running");
            return;
        }

        LoadVersionInfo();

        if (!MultiInstance::isBaseClientInstalled("Default")) {
            LOG_INFO("Default client not installed, downloading automatically...");

            if (!TryBeginClientUpdate("Default")) {
                LOG_WARN("Default client installation already in progress");
            } else {
                auto progressCallback = [](float progress, const std::string &msg) {
                    // LOG_INFO("Default download progress: {:.0f}% - {}", progress * 100.0f, msg);
                };

                auto completionCallback = [](bool success, const std::string &message) {
                    if (success) {
                        LOG_INFO("Default client installed successfully");
                        MarkClientAsInstalled("Default", GetClientVersion("Default"));
                    } else {
                        LOG_ERROR("Default client installation failed: {}", message);
                        ThreadTask::RunOnMain([message]() {
                            UpdateNotification::Show(
                                "Installation Failed",
                                std::format("Failed to install Default client: {}", message),
                                5.0f
                            );
                        });
                    }
                    EndClientUpdate("Default");
                };

                ClientManager::InstallClientAsync("Default", progressCallback, completionCallback);
            }
        }

        shouldStop.store(false);
        isRunning.store(true);

        checkerThread = std::thread(CheckerLoop);

        LOG_INFO("Client update checker initialized");
    }

    void UpdateChecker::Shutdown() {
        if (!isRunning.load()) {
            return;
        }

        LOG_INFO("Client update checker shutdown requested");

        shouldStop.store(true);

        {
            std::lock_guard lock(shutdownMutex);
            shutdownCV.notify_all();
        }

        if (checkerThread.joinable()) {
            checkerThread.join();
        }

        isRunning.store(false);

        LOG_INFO("Client update checker shutdown complete");
    }

    void UpdateChecker::CheckNow(const std::string &clientName) {
        ThreadTask::fireAndForget([clientName]() {
            CheckClientForUpdate(clientName);
        });
    }

    void UpdateChecker::CheckAllNow() {
        ThreadTask::fireAndForget([]() {
            for (const auto &clientName: g_availableClientsNames) {
                if (shouldStop.load()) {
                    break;
                }

                if (clientName != "Default" && !MultiInstance::isBaseClientInstalled(clientName)) {
                    continue;
                }

                CheckClientForUpdate(clientName);

                {
                    std::unique_lock lock(shutdownMutex);
                    if (shutdownCV.wait_for(lock, std::chrono::seconds(2), []() {
                            return shouldStop.load();
                        })) {
                        break;
                    }
                }
            }
        });
    }

    std::string UpdateChecker::GetClientVersion(const std::string &clientName) {
        try {
            if (clientName == "MacSploit") {
                auto version = ClientManager::GetMacsploitVersion();
                return version.relVersion;
            } else if (clientName == "Hydrogen") {
                auto version = ClientManager::GetHydrogenVersion();
                return version.macos.exploit_version.value_or("");
            } else if (clientName == "Delta") {
                return ClientManager::GetDeltaVersion();
            } else if (clientName == "Default") {
                return ClientManager::GetLatestRobloxVersion();
            }
        } catch (const std::exception &e) {
            LOG_ERROR("Failed to get version for {}: {}", clientName, e.what());
        }
        return "";
    }

    ClientVersionInfo UpdateChecker::GetVersionInfo(const std::string &clientName) {
        std::lock_guard lock(versionsMutex);
        auto it = clientVersions.find(clientName);
        if (it != clientVersions.end()) {
            return it->second;
        }
        return ClientVersionInfo {};
    }

    void UpdateChecker::MarkClientAsInstalled(const std::string &clientName, const std::string &version) {
        std::lock_guard lock(versionsMutex);
        auto &info = clientVersions[clientName];
        info.installedVersion = version;
        info.latestVersion = version;
        info.updateAvailable = false;
        info.lastChecked = std::chrono::system_clock::now();
        SaveVersionInfoLocked();
    }

} // namespace ClientUpdateChecker
