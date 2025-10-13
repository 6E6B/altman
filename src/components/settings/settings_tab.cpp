#include "settings.h"
#include <imgui.h>
#include <string>
#include <vector>

#include "../../utils/system/multi_instance.h"
#include "../components.h"
#include "../console/console.h"
#include "../data.h"
#include "core/app_state.h"

using namespace ImGui;

// Static flag to manage console modal visibility
static bool g_requestOpenConsoleModal = false;

void RenderSettingsTab() {
	// Button to open the Console modal (always visible)
	if (Button("Open Console")) { g_requestOpenConsoleModal = true; }
	Spacing();

	if (!g_accounts.empty()) {
		SeparatorText("Accounts");
		Text("Default Account:");

		// Build a list of all accounts for the dropdown (including banned-like)
		std::vector<std::string> accountLabels;
		std::vector<const char *> names;
		std::vector<size_t> idxMap;
		accountLabels.reserve(g_accounts.size());
		names.reserve(g_accounts.size());
		idxMap.reserve(g_accounts.size());

		int current_default_idx = -1;
		for (size_t i = 0; i < g_accounts.size(); ++i) {
			std::string label;
			if (g_accounts[i].displayName == g_accounts[i].username) {
				label = g_accounts[i].displayName;
			} else {
				label = g_accounts[i].displayName + " (" + g_accounts[i].username + ")";
			}
			accountLabels.push_back(label);
			names.push_back(accountLabels.back().c_str());
			idxMap.push_back(i);

			if (g_accounts[i].id == g_defaultAccountId) { current_default_idx = static_cast<int>(names.size() - 1); }
		}

		int combo_idx = current_default_idx;

		if (!names.empty()) {
			if (Combo("##defaultAccountCombo", &combo_idx, names.data(), static_cast<int>(names.size()))) {
				if (combo_idx >= 0 && combo_idx < static_cast<int>(idxMap.size())) {
					g_defaultAccountId = g_accounts[idxMap[combo_idx]].id;

					g_selectedAccountIds.clear();
					g_selectedAccountIds.insert(g_defaultAccountId);

					Data::SaveSettings("settings.json");
				}
			}
		} else {
			TextDisabled("No accounts available.");
		}

		Spacing();
		SeparatorText("General");
		int interval = g_statusRefreshInterval;
		if (InputInt("Status Refresh Interval (min)", &interval)) {
			if (interval < 1) { interval = 1; }
			if (interval != g_statusRefreshInterval) {
				g_statusRefreshInterval = interval;
				Data::SaveSettings("settings.json");
			}
		}

		bool checkUpdates = g_checkUpdatesOnStartup;
		if (Checkbox("Check for updates on startup", &checkUpdates)) {
			g_checkUpdatesOnStartup = checkUpdates;
			Data::SaveSettings("settings.json");
		}

		Spacing();
		SeparatorText("Launch Options");
		bool multi = g_multiRobloxEnabled;
		if (Checkbox("Multi Roblox", &multi)) {
			g_multiRobloxEnabled = multi;
#ifdef _WIN32
			if (g_multiRobloxEnabled) {
				MultiInstance::Enable();
			} else {
				MultiInstance::Disable();
			}
#endif
			Data::SaveSettings("settings.json");
		}

		BeginDisabled(g_multiRobloxEnabled);
		bool killOnLaunch = g_killRobloxOnLaunch;
		if (Checkbox("Kill Roblox When Launching", &killOnLaunch)) {
			g_killRobloxOnLaunch = killOnLaunch;
			Data::SaveSettings("settings.json");
		}
		bool clearOnLaunch = g_clearCacheOnLaunch;
		if (Checkbox("Clear Roblox Cache When Launching", &clearOnLaunch)) {
			g_clearCacheOnLaunch = clearOnLaunch;
			Data::SaveSettings("settings.json");
		}
		EndDisabled();
	} else {
		TextDisabled("No accounts available to set a default.");
	}

	// Handle Console modal rendering
	if (g_requestOpenConsoleModal) {
		OpenPopup("ConsolePopup");
		g_requestOpenConsoleModal = false;
	}

	// Size the console popup to 60%% of viewport width and 80%% of viewport height every time it opens
	const ImGuiViewport *vp = GetMainViewport();
	ImVec2 desiredSize(vp->WorkSize.x * 0.60f, vp->WorkSize.y * 0.80f);
	SetNextWindowSize(desiredSize, ImGuiCond_Always);

	if (BeginPopupModal("ConsolePopup", nullptr, ImGuiWindowFlags_NoResize)) {
		ImGuiStyle &style = GetStyle();

		float closeBtnWidth = CalcTextSize("Close").x + style.FramePadding.x * 2.0f;
		float closeBtnHeight = GetFrameHeight();

		ImVec2 avail = GetContentRegionAvail();
		float childHeight = avail.y - closeBtnHeight - style.ItemSpacing.y;
		if (childHeight < 0) { childHeight = 0; }

		BeginChild("ConsoleArea", ImVec2(0, childHeight), ImGuiChildFlags_Border);
		Console::RenderConsoleTab();
		EndChild();

		Spacing();

		// Align the close button to the right
		SetCursorPosX(GetContentRegionMax().x - closeBtnWidth);
		if (Button("Close", ImVec2(closeBtnWidth, 0))) { CloseCurrentPopup(); }

		EndPopup();
	}
}
