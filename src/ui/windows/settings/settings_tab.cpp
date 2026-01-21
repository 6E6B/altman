#include "settings.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <format>
#include <imgui.h>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "components/data.h"
#include "system/multi_instance.h"
#include "system/roblox_control.h"
#include "ui/widgets/modal_popup.h"
#include "console/console.h"
#include "utils/paths.h"
#include "utils/thread_task.h"

#ifdef __APPLE__
#include "network/ipa_installer_macos.h"
#include "network/client_manager_macos.h"
#endif

static bool g_requestOpenConsoleModal = false;

#ifdef __APPLE__
static constexpr std::array<std::string_view, 4> g_availableClientsNames = {
    "Default",
    "MacSploit",
    "Hydrogen",
    "Delta"
};

struct ClientInstallState {
    bool isInstalling = false;
    std::string currentClient;
    float progress = 0.0f;
    std::string statusMessage;
};

static ClientInstallState g_installState;
static std::mutex g_installMutex;

namespace {
	void OpenAccountDocumentsFolder(const AccountData& acc) {
		const auto appDataDir = AltMan::Paths::AppData();
		if (appDataDir.empty()) return;

		const std::string documentsPath = std::format("{}/environments/{}/Documents",
													  appDataDir.string(), acc.username);

		std::error_code ec;
		if (!std::filesystem::exists(documentsPath, ec)) {
			LOG_INFO("Documents folder does not exist: {}", documentsPath);
			return;
		}

		const std::string command = std::format("open \"{}\"", documentsPath);
		const int result = std::system(command.c_str());

		if (result == 0) {
			LOG_INFO("Opened documents folder for: {}", acc.username);
		} else {
			LOG_ERROR("Failed to open documents folder for: {}", acc.username);
		}
	}

	void OpenAccountEnvironmentFolder(const AccountData& acc) {
		const auto appDataDir = AltMan::Paths::AppData();
		if (appDataDir.empty()) return;

		const std::string envPath = std::format("{}/environments/{}", appDataDir.string(), acc.username);

		std::error_code ec;
		if (!std::filesystem::exists(envPath, ec)) {
			LOG_INFO("Environment folder does not exist: {}", envPath);
			return;
		}

		const std::string command = std::format("open \"{}\"", envPath);
		const int result = std::system(command.c_str());

		if (result == 0) {
			LOG_INFO("Opened environment folder for: {}", acc.username);
		} else {
			LOG_ERROR("Failed to open environment folder for: {}", acc.username);
		}
	}

	template <typename Fn>
	void ProcessSelectedAccounts(Fn&& operation) {
		for (const int accountId : g_selectedAccountIds) {
			if (AccountData* acc = getAccountById(accountId)) {
				operation(*acc);
			}
		}
	}

    void RenderClientSelector() {
		const auto& availableClientsForUI = MultiInstance::getAvailableClientsForUI(false);

		if (availableClientsForUI.empty()) {
			ImGui::TextDisabled("No clients available");
			ImGui::SameLine();
			if (ImGui::Button("Install Clients")) {
				ImGui::SetScrollHereY(1.0f);
			}
			return;
		}

		for (const int accountId : g_selectedAccountIds) {
			AccountData* acc = getAccountById(accountId);
			if (!acc)
				continue;

			std::string currentBase = acc->customClientBase.empty() ? "Default" : acc->customClientBase;

			ImGui::PushID(acc->id);

			ImGui::Text("%s:", acc->username.c_str());
			ImGui::SameLine();

			ImGui::SetNextItemWidth(150.0f);
			if (ImGui::BeginCombo("##ClientSelect", currentBase.c_str())) {
				for (const auto& clientName : availableClientsForUI) {
					const bool isInstalled = MultiInstance::isBaseClientInstalled(clientName);

					if (!isInstalled && clientName != "Default") {
						ImGui::BeginDisabled();
					}

					bool isSelected = (currentBase == clientName);

					if (ImGui::Selectable(clientName.c_str(), isSelected)) {
						acc->customClientBase = (clientName == "Default") ? "" : clientName;
						Data::SaveAccounts();
						LOG_INFO("Set {} to use base client: {}", acc->username, clientName);
					}

					if (isSelected) {
						ImGui::SetItemDefaultFocus();
					}

					if (!isInstalled && clientName != "Default") {
						ImGui::EndDisabled();
					}
				}
				ImGui::EndCombo();
			}

			ImGui::SameLine();
			std::string userClientName = "Roblox_" + acc->username;
			bool userCopyExists = MultiInstance::isClientInstalled(acc->username, userClientName);

			if (userCopyExists) {
				ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Ready");
			} else {
				ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Will copy on launch");
			}

			ImGui::PopID();
		}
	}
}
#endif // __APPLE__

void RenderSettingsTab() {
	ImGui::BeginChild("SettingsScrollRegion", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);

	if (ImGui::Button("Open Console")) {
		g_requestOpenConsoleModal = true;
	}
	ImGui::Spacing();

	ImGui::SeparatorText("General");

	int interval = g_statusRefreshInterval;
	if (ImGui::InputInt("Status Refresh Interval (min)", &interval)) {
		interval = std::max(1, interval);
		if (interval != g_statusRefreshInterval) {
			g_statusRefreshInterval = interval;
			Data::SaveSettings();
		}
	}

	bool checkUpdates = g_checkUpdatesOnStartup;
	if (ImGui::Checkbox("Check for updates on startup", &checkUpdates)) {
		g_checkUpdatesOnStartup = checkUpdates;
		Data::SaveSettings();
	}

	ImGui::Spacing();
	ImGui::SeparatorText("Launch Options");

	bool multi = g_multiRobloxEnabled;
	if (ImGui::Checkbox("Multi Roblox", &multi)) {
#ifdef _WIN32
		if (multi && RobloxControl::IsRobloxRunning()) {
			ModalPopup::AddYesNo(
				"Enabling Multi Roblox requires closing all running Roblox instances.\n\n"
				"Do you want to continue?",
				[]() {
					RobloxControl::KillRobloxProcesses();
					ThreadTask::fireAndForget([] {
						constexpr int maxAttempts = 50;
						constexpr int delayMs = 100;

						for (int i = 0; i < maxAttempts; ++i) {
							if (!RobloxControl::IsRobloxRunning()) {
								g_multiRobloxEnabled = true;
								MultiInstance::Enable();
								Data::SaveSettings();
								LOG_INFO("Multi Roblox enabled after Roblox exit");
								return;
							}
							std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
						}
						LOG_ERROR("Timed out waiting for Roblox to exit");
					});
				}
			);
		} else {
			g_multiRobloxEnabled = multi;
			if (g_multiRobloxEnabled) {
				MultiInstance::Enable();
			} else {
				MultiInstance::Disable();
			}
			Data::SaveSettings();
		}
#else
		// Mutex does not work MacOS but we save the option anyways
		g_multiRobloxEnabled = multi;
		Data::SaveSettings();
#endif
	}

#ifdef _WIN32
	if (ImGui::IsItemHovered()) {
		if (g_multiRobloxEnabled) {
			ImGui::SetTooltip("AltMan must be running before launching Roblox for multi-instance to work.");
		} else {
			ImGui::SetTooltip("Enabling this will close any running Roblox instances.");
		}
	}
#endif

	ImGui::BeginDisabled(g_multiRobloxEnabled);

	bool killOnLaunch = g_killRobloxOnLaunch;
	if (ImGui::Checkbox("Kill Roblox When Launching", &killOnLaunch)) {
		g_killRobloxOnLaunch = killOnLaunch;
		Data::SaveSettings();
	}

	bool clearOnLaunch = g_clearCacheOnLaunch;
	if (ImGui::Checkbox("Clear Roblox Cache When Launching", &clearOnLaunch)) {
		g_clearCacheOnLaunch = clearOnLaunch;
		Data::SaveSettings();
	}

	ImGui::EndDisabled();

	ImGui::Spacing();

	if (!g_accounts.empty()) {
		ImGui::SeparatorText("Accounts");
		ImGui::Text("Default Account:");

		std::vector<std::string> accountLabels;
		std::vector<const char*> names;
		std::vector<size_t> idxMap;

		accountLabels.reserve(g_accounts.size());
		names.reserve(g_accounts.size());
		idxMap.reserve(g_accounts.size());

		int current_default_idx = -1;

		for (size_t i = 0; i < g_accounts.size(); ++i) {
			const auto& acc = g_accounts[i];
			const std::string label = (acc.displayName == acc.username)
				? acc.displayName
				: std::format("{} ({})", acc.displayName, acc.username);

			accountLabels.push_back(label);
			names.push_back(accountLabels.back().c_str());
			idxMap.push_back(i);

			if (acc.id == g_defaultAccountId) {
				current_default_idx = static_cast<int>(names.size() - 1);
			}
		}

		int combo_idx = current_default_idx;

		if (!names.empty()) {
			if (ImGui::Combo("##defaultAccountCombo", &combo_idx, names.data(),
						   static_cast<int>(names.size()))) {
				if (combo_idx >= 0 && combo_idx < static_cast<int>(idxMap.size())) {
					g_defaultAccountId = g_accounts[idxMap[combo_idx]].id;
					g_selectedAccountIds.clear();
					g_selectedAccountIds.insert(g_defaultAccountId);
					Data::SaveSettings();
				}
						   }
		} else {
			ImGui::TextDisabled("No accounts available.");
		}
	} else {
		ImGui::TextDisabled("No accounts available to set a default.");
	}

	ImGui::Spacing();

#ifdef __APPLE__
	ImGui::SeparatorText("Selected Account Settings");

	if (g_selectedAccountIds.empty()) {
		ImGui::TextDisabled("Select accounts from the Accounts tab to configure");
	} else {
		if (ImGui::CollapsingHeader("Folders", ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGui::Button("Open Documents Folder")) {
				ProcessSelectedAccounts(OpenAccountDocumentsFolder);
			}

			ImGui::SameLine();

			if (ImGui::Button("Open Environment Folder")) {
				ProcessSelectedAccounts(OpenAccountEnvironmentFolder);
			}
		}

		ImGui::Spacing();

		if (ImGui::CollapsingHeader("Client Configuration", ImGuiTreeNodeFlags_DefaultOpen)) {
			RenderClientSelector();
		}
	}

	ImGui::Spacing();
	ImGui::SeparatorText("Client Management");

	bool forceLatest = g_forceLatestRobloxVersion;
	if (ImGui::Checkbox("Force Latest Roblox Version For Clients", &forceLatest)) {
		g_forceLatestRobloxVersion = forceLatest;
		Data::SaveSettings();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("If your client crashed on startup try enabling this option, then remove and install client again.\nWhen enabled, ignores client-recommended versions and always uses the latest Roblox version.\nMay cause compatibility issues with some clients.\nDefault client remains non affected.");
	}

	const float availHeight = ImGui::GetContentRegionAvail().y;
	const auto& style = ImGui::GetStyle();

	float totalReserved = 0.0f;
	const float buttonRowHeight = ImGui::GetFrameHeight() + style.ItemSpacing.y;
	totalReserved += buttonRowHeight;

	if (g_installState.isInstalling) {
		const float progressHeight = ImGui::GetFrameHeight() * 2.0f + style.ItemSpacing.y * 2.0f;
		totalReserved += progressHeight;
	}

	constexpr float MIN_TABLE_HEIGHT_MULTIPLIER = 6.0f;
	const float minTableHeight = ImGui::GetFrameHeight() * MIN_TABLE_HEIGHT_MULTIPLIER;

	float tableHeight = std::max(minTableHeight, availHeight - totalReserved);
	if (availHeight <= totalReserved) {
		tableHeight = minTableHeight;
	}

	if (ImGui::BeginTable("ClientTable", 4,
		ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
		ImVec2(0,tableHeight))) {
		ImGui::TableSetupColumn("Client", ImGuiTableColumnFlags_WidthFixed, 100.0f);
		ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100.0f);
		ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 80.0f);
		ImGui::TableHeadersRow();

		// Static map to store buffers per client - initialized once
		static std::unordered_map<std::string, std::array<char, 256>> keyBuffers;
		static std::unordered_map<std::string, bool> buffersInitialized;

		for (std::string_view clientName : g_availableClientsNames) {
			const std::string clientStr(clientName);
			const bool isInstalled = MultiInstance::isBaseClientInstalled(clientStr);
			const bool needsKey = (clientName != "Default") && (clientName != "MacSploit");

			ImGui::TableNextRow();
			ImGui::PushID(clientStr.c_str());

			ImGui::TableNextColumn();
			ImGui::Text("%s", clientStr.c_str());

			ImGui::TableNextColumn();
			if (needsKey) {
				auto& key = g_clientKeys[clientStr];

				// Initialize buffer only once per client
				if (!buffersInitialized[clientStr]) {
					auto& buffer = keyBuffers[clientStr];
					std::strncpy(buffer.data(), key.c_str(), buffer.size() - 1);
					buffer.back() = '\0';
					buffersInitialized[clientStr] = true;
				}

				auto& buffer = keyBuffers[clientStr];

				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::InputText("##Key", buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
					key = std::string(buffer.data());
					Data::SaveSettings();
					LOG_INFO("Updated key for {}", clientStr);
				}

				if (ImGui::IsItemDeactivatedAfterEdit()) {
					key = std::string(buffer.data());
					Data::SaveSettings();
					LOG_INFO("Updated key for {}", clientStr);
				}
			} else {
				ImGui::TextDisabled("No key required");
			}

			ImGui::TableNextColumn();
			if (isInstalled) {
				ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Installed");
			} else {
				ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Not Installed");
			}

			ImGui::TableNextColumn();

			const bool disableInstall = needsKey && g_clientKeys[clientStr].empty();
			const bool disableButton = g_installState.isInstalling || disableInstall;

			if (disableButton) {
				ImGui::BeginDisabled();
			}

			if (isInstalled) {
				if (ImGui::Button("Remove", ImVec2(-FLT_MIN, 0))) {
					{
						std::lock_guard<std::mutex> lock(g_installMutex);
						g_installState.isInstalling = true;
						g_installState.currentClient = clientStr;
						g_installState.statusMessage = "Removing client...";
					}

					ClientManager::RemoveClientAsync(clientStr, [](bool success, const std::string& message) {
						std::lock_guard<std::mutex> lock(g_installMutex);
						g_installState.isInstalling = false;
						if (success) {
							LOG_INFO(message);
							g_installState.statusMessage = "Client removed successfully";
						} else {
							LOG_ERROR(message);
							g_installState.statusMessage = std::format("Error: {}", message);
						}

						// Refresh client list
						MultiInstance::getAvailableClientsForUI(true);
					});
				}
			} else {
				if (ImGui::Button("Install", ImVec2(-FLT_MIN, 0))) {
					{
						std::lock_guard<std::mutex> lock(g_installMutex);
						g_installState.isInstalling = true;
						g_installState.currentClient = clientStr;
						g_installState.progress = 0.0f;
						g_installState.statusMessage = "Starting installation...";
					}

					auto progressCallback = [](float progress, const std::string& message) {
						std::lock_guard<std::mutex> lock(g_installMutex);
						g_installState.progress = progress;
						g_installState.statusMessage = message;
					};

					auto completionCallback = [](bool success, const std::string& message) {
						std::lock_guard<std::mutex> lock(g_installMutex);
						g_installState.isInstalling = false;
						if (success) {
							LOG_INFO(message);
							g_installState.statusMessage = "Installation complete!";
						} else {
							LOG_ERROR(message);
							g_installState.statusMessage = std::format("Error: {}", message);
						}

						// Refresh client list
						MultiInstance::getAvailableClientsForUI(true);
					};

					ClientManager::InstallClientAsync(g_installState.currentClient,
													 progressCallback,
													 completionCallback);
				}

				if (disableInstall && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
					ImGui::SetTooltip("Please enter a key before installing");
				}
			}

			if (disableButton) {
				ImGui::EndDisabled();
			}

			ImGui::PopID();
		}

		ImGui::EndTable();
	}

	ImGui::Separator();

	{
		std::lock_guard<std::mutex> lock(g_installMutex);
		if (g_installState.isInstalling) {
			ImGui::Separator();
			ImGui::Text("Installing %s...", g_installState.currentClient.c_str());
			ImGui::ProgressBar(g_installState.progress, ImVec2(-1, 0));
			ImGui::TextWrapped("%s", g_installState.statusMessage.c_str());
		}
	}
#endif // __APPLE__

	ImGui::EndChild();

	if (g_requestOpenConsoleModal) {
		ImGui::OpenPopup("ConsolePopup");
		g_requestOpenConsoleModal = false;
	}

	const ImGuiViewport* vp = ImGui::GetMainViewport();
	const ImVec2 desiredSize(vp->WorkSize.x * 0.60f, vp->WorkSize.y * 0.80f);
	ImGui::SetNextWindowSize(desiredSize, ImGuiCond_Always);

	if (ImGui::BeginPopupModal("ConsolePopup", nullptr, ImGuiWindowFlags_NoResize)) {
		const ImGuiStyle& style = ImGui::GetStyle();

		const float closeBtnWidth = ImGui::CalcTextSize("Close").x + style.FramePadding.x * 2.0f;
		const float closeBtnHeight = ImGui::GetFrameHeight();

		const ImVec2 avail = ImGui::GetContentRegionAvail();
		const float childHeight = std::max(0.0f, avail.y - closeBtnHeight - style.ItemSpacing.y);

		ImGui::BeginChild("ConsoleArea", ImVec2(0, childHeight), ImGuiChildFlags_Borders);
		Console::RenderConsoleTab();
		ImGui::EndChild();

		ImGui::Spacing();

		ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - closeBtnWidth);
		if (ImGui::Button("Close", ImVec2(closeBtnWidth, 0))) {
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}
