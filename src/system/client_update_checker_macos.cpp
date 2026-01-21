#include "client_update_checker_macos.h"

#include <format>
#include <fstream>
#include <nlohmann/json.hpp>

#include "auto_updater.h"
#include "network/client_manager_macos.h"
#include "system/multi_instance.h"
#include "console/console.h"
#include "utils/thread_task.h"
#include "utils/paths.h"
#include "ui/widgets/notifications.h"
#include "components/data.h"

namespace ClientUpdateChecker {

namespace {
    std::unordered_map<std::string, ClientVersionInfo> clientVersions;
    std::atomic<bool> isRunning{false};
    std::atomic<bool> shouldStop{false};
    std::thread checkerThread;
    std::filesystem::path configPath;
}

std::filesystem::path UpdateChecker::GetConfigPath() {
	return AltMan::Paths::Config("client_versions.json");
}

void UpdateChecker::SaveVersionInfo() {
    if (configPath.empty()) {
        configPath = GetConfigPath();
    }

    nlohmann::json j = nlohmann::json::object();
    
    for (const auto& [clientName, info] : clientVersions) {
        j[clientName] = {
            {"installedVersion", info.installedVersion},
            {"latestVersion", info.latestVersion},
            {"lastChecked", std::chrono::system_clock::to_time_t(info.lastChecked)},
            {"updateAvailable", info.updateAvailable}
        };
    }

    std::ofstream file(configPath);
    if (file.is_open()) {
        file << j.dump(2);
        file.close();
    }
}

void UpdateChecker::LoadVersionInfo() {
    if (configPath.empty()) {
        configPath = GetConfigPath();
    }

    if (!std::filesystem::exists(configPath)) {
        return;
    }

    std::ifstream file(configPath);
    if (!file.is_open()) {
        return;
    }

    try {
        nlohmann::json j;
        file >> j;
        file.close();

        for (auto& [clientName, data] : j.items()) {
            ClientVersionInfo info;
            info.installedVersion = data.value("installedVersion", "");
            info.latestVersion = data.value("latestVersion", "");
            info.updateAvailable = data.value("updateAvailable", false);
            
            auto lastCheckedTime = data.value("lastChecked", 0LL);
            info.lastChecked = std::chrono::system_clock::from_time_t(lastCheckedTime);
            
            clientVersions[clientName] = info;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to load client versions: {}", e.what());
    }
}

void UpdateChecker::CheckClientForUpdate(const std::string& clientName) {
    if (!MultiInstance::isBaseClientInstalled(clientName)) {
        return;
    }

    LOG_INFO("Checking for updates: {}", clientName);

    auto& info = clientVersions[clientName];
    std::string latestVersion = GetClientVersion(clientName);

    if (latestVersion.empty()) {
        LOG_WARN("Failed to fetch latest version for {}", clientName);
        return;
    }

    info.latestVersion = latestVersion;
    info.lastChecked = std::chrono::system_clock::now();

    if (info.installedVersion.empty()) {
        info.installedVersion = latestVersion;
        info.updateAvailable = false;
        LOG_INFO("{} version initialized: {}", clientName, latestVersion);
        SaveVersionInfo();
        return;
    }

    if (info.installedVersion != latestVersion) {
        info.updateAvailable = true;
        LOG_INFO("{} update available: {} -> {}",
            clientName, info.installedVersion, latestVersion);
        
        SaveVersionInfo();

        ThreadTask::RunOnMain([clientName, info]() {
            NotifyAndUpdate(clientName, info);
        });
    } else {
        info.updateAvailable = false;
        LOG_INFO("{} is up to date: {}", clientName, latestVersion);
        SaveVersionInfo();
    }
}

void UpdateChecker::NotifyAndUpdate(const std::string& clientName, const ClientVersionInfo& info) {
    std::string message = std::format("Updating {} from {} to {}...", 
        clientName, info.installedVersion, info.latestVersion);

    UpdateNotification::Show("Client Update", message, 5.0f);
    
    LOG_INFO("Starting auto-update for {}", clientName);

    std::string clientPath = MultiInstance::getBaseClientPath(clientName);
    if (!clientPath.empty() && std::filesystem::exists(clientPath)) {
        std::error_code ec;
        std::filesystem::remove_all(clientPath, ec);
        if (ec) {
            LOG_ERROR("Failed to remove old {}: {}", clientName, ec.message());
            UpdateNotification::Show("Update Failed", 
                std::format("Failed to remove old {} client", clientName), 5.0f);
            return;
        }
    }

    auto progressCallback = [clientName](float progress, const std::string& msg) {
        //LOG_INFO("{} update progress: {:.0f}% - {}", clientName, progress * 100.0f, msg);
    };

    auto completionCallback = [clientName](bool success, const std::string& message) {
        if (success) {
            LOG_INFO("{} updated successfully", clientName);

            auto& info = clientVersions[clientName];
            info.installedVersion = info.latestVersion;
            info.updateAvailable = false;
            SaveVersionInfo();
            
            ThreadTask::RunOnMain([clientName]() {
                UpdateNotification::Show("Update Complete", 
                    std::format("{} has been updated successfully!", clientName), 5.0f);
            });
        } else {
            LOG_ERROR("{} update failed: {}", clientName, message);
            
            ThreadTask::RunOnMain([clientName, message]() {
                UpdateNotification::Show("Update Failed", 
                    std::format("{}: {}", clientName, message), 5.0f);
            });
        }
    };

    ClientManager::InstallClientAsync(clientName, progressCallback, completionCallback);
}

void UpdateChecker::CheckerLoop() {
    LOG_INFO("Client update checker started");

    while (!shouldStop) {
        
        for (const auto& clientName : g_availableClientsNames) {
            if (shouldStop) break;

            if (clientName != "Default" && !MultiInstance::isBaseClientInstalled(clientName)) {
                continue;
            }

            auto& info = clientVersions[clientName];
            auto now = std::chrono::system_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::hours>(
                now - info.lastChecked).count();

            if (elapsed >= 24 || info.lastChecked == std::chrono::system_clock::time_point{}) {
                CheckClientForUpdate(clientName);
            }

            std::this_thread::sleep_for(std::chrono::seconds(2));
        }

        // Sleep for 1 hour before next check cycle
        for (int i = 0; i < 3600 && !shouldStop; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    LOG_INFO("Client update checker stopped");
}

void UpdateChecker::Initialize() {
    if (isRunning) {
        LOG_WARN("Client update checker already running");
        return;
    }

    LoadVersionInfo();

	if (!MultiInstance::isBaseClientInstalled("Default")) {
		LOG_INFO("Default client not installed, downloading automatically...");

		auto progressCallback = [](float progress, const std::string& msg) {
			// LOG_INFO("Default download progress: {:.0f}% - {}", progress * 100.0f, msg);
		};

		auto completionCallback = [](bool success, const std::string& message) {
			if (success) {
				LOG_INFO("Default client installed successfully");
				MarkClientAsInstalled("Default", GetClientVersion("Default"));
			} else {
				LOG_ERROR("Default client installation failed: {}", message);
				ThreadTask::RunOnMain([message]() {
					UpdateNotification::Show("Installation Failed",
						std::format("Failed to install Default client: {}", message), 5.0f);
				});
			}
		};

		ClientManager::InstallClientAsync("Default", progressCallback, completionCallback);
	}
    
    shouldStop = false;
    isRunning = true;
    
    checkerThread = std::thread(CheckerLoop);
    checkerThread.detach();
    
    LOG_INFO("Client update checker initialized");
}

void UpdateChecker::Shutdown() {
    if (!isRunning) {
        return;
    }

    shouldStop = true;
    isRunning = false;
    
   LOG_INFO("Client update checker shutdown requested");
}

void UpdateChecker::CheckNow(const std::string& clientName) {
    ThreadTask::fireAndForget([clientName]() {
        CheckClientForUpdate(clientName);
    });
}

void UpdateChecker::CheckAllNow() {
    ThreadTask::fireAndForget([]() {
        for (const auto& clientName : g_availableClientsNames) {
            if (clientName != "Default" && !MultiInstance::isBaseClientInstalled(clientName)) {
                continue;
            }
            CheckClientForUpdate(clientName);
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    });
}

std::string UpdateChecker::GetClientVersion(const std::string& clientName) {
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
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to get version for {}: {}", clientName, e.what());
    }
    return "";
}

ClientVersionInfo UpdateChecker::GetVersionInfo(const std::string& clientName) {
    auto it = clientVersions.find(clientName);
    if (it != clientVersions.end()) {
        return it->second;
    }
    return ClientVersionInfo{};
}

void UpdateChecker::MarkClientAsInstalled(const std::string& clientName, const std::string& version) {
    auto& info = clientVersions[clientName];
    info.installedVersion = version;
    info.latestVersion = version;
    info.updateAvailable = false;
    info.lastChecked = std::chrono::system_clock::now();
    SaveVersionInfo();
}

}