#include "settings.h"
#include <imgui.h>
#include <vector>
#include <string>

#include "../components.h"
#include "../data.h"
#include "core/app_state.h"

using namespace ImGui;
using namespace std;

void RenderSettingsTab()
{
        if (!g_accounts.empty())
        {
                Text("Default Account:");

		vector<const char*> names;
		names.reserve(g_accounts.size());
		int current_default_idx = -1;

		for (size_t i = 0; i < g_accounts.size(); ++i)
		{
			names.push_back(g_accounts[i].displayName.c_str());
			if (g_accounts[i].id == g_defaultAccountId)
			{
				current_default_idx = static_cast<int>(i);
			}
		}

		if (current_default_idx == -1 && !g_accounts.empty())
		{
		}

		int combo_idx = current_default_idx;
                if (Combo("##defaultAccountCombo", &combo_idx, names.data(), static_cast<int>(names.size())))
                {
                        if (combo_idx >= 0 && combo_idx < static_cast<int>(g_accounts.size()))
                        {
                                g_defaultAccountId = g_accounts[combo_idx].id;

                                g_selectedAccountIds.clear();
                                g_selectedAccountIds.insert(g_defaultAccountId);

                                Data::SaveSettings("settings.json");
                        }
                }

                int interval = g_statusRefreshInterval;
                if (InputInt("Status Refresh Interval (min)", &interval))
                {
                        if (interval < 1) interval = 1;
                        if (interval != g_statusRefreshInterval)
                        {
                                g_statusRefreshInterval = interval;
                                Data::SaveSettings("settings.json");
                        }
                }

                bool checkUpdates = g_checkUpdatesOnStartup;
                if (Checkbox("Check for updates on startup", &checkUpdates))
                {
                        g_checkUpdatesOnStartup = checkUpdates;
                        Data::SaveSettings("settings.json");
                }

                bool multi = g_multiRobloxEnabled;
                if (Checkbox("Multi Roblox", &multi))
                {
                        g_multiRobloxEnabled = multi;
                        if (g_multiRobloxEnabled)
                                RobloxControl::EnableMultiInstance();
                        else
                                RobloxControl::DisableMultiInstance();
                        Data::SaveSettings("settings.json");
                }

                BeginDisabled(g_multiRobloxEnabled);
                bool killOnLaunch = g_killRobloxOnLaunch;
                if (Checkbox("Kill Roblox When Launching", &killOnLaunch))
                {
                        g_killRobloxOnLaunch = killOnLaunch;
                        Data::SaveSettings("settings.json");
                }
                bool clearOnLaunch = g_clearCacheOnLaunch;
                if (Checkbox("Clear Roblox Cache When Launching", &clearOnLaunch))
                {
                        g_clearCacheOnLaunch = clearOnLaunch;
                        Data::SaveSettings("settings.json");
                }
                EndDisabled();
        }
        else
        {
                TextDisabled("No accounts available to set a default.");
        }
}
