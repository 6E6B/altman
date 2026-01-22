#pragma once

#include <string>
#include <chrono>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace ClientUpdateChecker {

	struct ClientVersionInfo {
		std::string installedVersion;
		std::string latestVersion;
		std::chrono::system_clock::time_point lastChecked;
		bool updateAvailable{false};
	};

	class UpdateChecker {
		public:
			static void Initialize();
			static void Shutdown();

			static void CheckNow(const std::string& clientName);
			static void CheckAllNow();

			static std::string GetClientVersion(const std::string& clientName);
			static ClientVersionInfo GetVersionInfo(const std::string& clientName);

			static void MarkClientAsInstalled(const std::string& clientName, const std::string& version);

			[[nodiscard]] static bool IsClientUpdating(const std::string& clientName);

			static std::filesystem::path GetConfigPath();

		private:
			static void SaveVersionInfo();
			static void SaveVersionInfoLocked();
			static void LoadVersionInfo();

			static void CheckClientForUpdate(const std::string& clientName);
			static void NotifyAndUpdate(const std::string& clientName, const ClientVersionInfo& info);
			static void CheckerLoop();

			static bool TryBeginClientUpdate(const std::string& clientName);
			static void EndClientUpdate(const std::string& clientName);
	};

}