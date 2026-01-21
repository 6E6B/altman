#include "ui/ui.h"

#include "progress_overlay.h"

#include <algorithm>
#include <array>
#include <imgui.h>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "accounts/accounts.h"
#include "ui/windows/avatar/inventory.h"
#include "ui/windows/components.h"
#include "ui/widgets/bottom_right_status.h"
#include "ui/windows/friends/friends.h"
#include "ui/windows/games/games.h"
#include "ui/windows/history/history.h"
#include "network/roblox/common.h"
#include "network/roblox/auth.h"
#include "network/roblox/games.h"
#include "network/roblox/session.h"
#include "network/roblox/social.h"

#include "ui/windows/settings/settings.h"
#include "ui/widgets/modal_popup.h"
#include "ui/widgets/notifications.h"
#include "ui/widgets/progress_overlay.h"
#include "ui/widgets/bottom_right_status.h"

namespace {
    constexpr std::string_view ICON_ACCOUNTS = "\xEF\x80\x87";
    constexpr std::string_view ICON_FRIENDS = "\xEF\x83\x80";
    constexpr std::string_view ICON_GAMES = "\xEF\x84\x9B";
    constexpr std::string_view ICON_SERVERS = "\xEF\x88\xB3";
    constexpr std::string_view ICON_INVENTORY = "\xEF\x8A\x90";
    constexpr std::string_view ICON_HISTORY = "\xEF\x85\x9C";
    constexpr std::string_view ICON_SETTINGS = "\xEF\x80\x93";

    struct TabInfo {
        const char* title;
        Tab tabId;
        void (*renderFunction)();
    };

    constexpr std::array TABS = {
        TabInfo{"\xEF\x80\x87  Accounts", Tab_Accounts, RenderFullAccountsTabContent},
        TabInfo{"\xEF\x83\x80  Friends", Tab_Friends, RenderFriendsTab},
        TabInfo{"\xEF\x84\x9B  Games", Tab_Games, RenderGamesTab},
        TabInfo{"\xEF\x88\xB3  Servers", Tab_Servers, RenderServersTab},
        TabInfo{"\xEF\x8A\x90  Inventory", Tab_Inventory, RenderInventoryTab},
        TabInfo{"\xEF\x85\x9C  History", Tab_History, RenderHistoryTab},
        TabInfo{"\xEF\x80\x93  Settings", Tab_Settings, RenderSettingsTab}
    };

    struct AccountDisplayInfo {
        std::string label;
        ImVec4 color;
    };

	std::vector<AccountDisplayInfo> getSelectedAccountsInfo() {
		std::vector<AccountDisplayInfo> result;
		result.reserve(g_selectedAccountIds.size());

		for (const int id : g_selectedAccountIds) {
			if (const AccountData* acc = getAccountById(id)) {
				const auto& label = acc->displayName.empty() ? acc->username : acc->displayName;
				result.push_back({label, getStatusColor(acc->status)});
			}
		}

		return result;
	}

    void renderTabBar() {
	    auto& style = ImGui::GetStyle();
    	style.FrameRounding = 2.5f;
    	style.ChildRounding = 2.5f;

    	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
			ImVec2(style.FramePadding.x + 2.0f, style.FramePadding.y + 2.0f));

    	if (ImGui::BeginTabBar("MainTabBar", ImGuiTabBarFlags_Reorderable)) {
    		for (const auto& tab : TABS) {
    			const ImGuiTabItemFlags flags = (g_activeTab == tab.tabId) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;

    			bool opened = ImGui::BeginTabItem(tab.title, nullptr, flags);

    			if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
    				g_activeTab = tab.tabId;

    			if (opened)
    			{
    				tab.renderFunction();
    				ImGui::EndTabItem();
    			}
    		}
    		ImGui::EndTabBar();
    	}

    	ImGui::PopStyleVar();
    }

    void renderSelectedAccountsStatus(const std::vector<AccountDisplayInfo>& accounts) {
        ImGui::TextUnformatted("Selected: ");
        ImGui::SameLine(0, 0);

        for (std::size_t i = 0; i < accounts.size(); ++i) {
            if (i > 0) {
                ImGui::TextUnformatted(", ");
                ImGui::SameLine(0, 0);
            }

            ImGui::PushStyleColor(ImGuiCol_Text, accounts[i].color);
            ImGui::TextUnformatted(accounts[i].label.c_str());
            ImGui::PopStyleColor();

            // Show asterisk for primary account
            if (i == 0 && accounts.size() > 1) {
                ImGui::SameLine(0, 0);
                ImGui::TextUnformatted("*");
            }

            // Continue on same line if not last
            if (i + 1 < accounts.size()) {
                ImGui::SameLine(0, 0);
            }
        }
    }

    void renderStatusBar(const ImGuiViewport* viewport) {
        const ImVec2 position(
            viewport->WorkPos.x + viewport->WorkSize.x,
            viewport->WorkPos.y + viewport->WorkSize.y
        );

        ImGui::SetNextWindowPos(position, ImGuiCond_Always, ImVec2(1, 1));

        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoFocusOnAppearing;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

        if (ImGui::Begin("StatusBar", nullptr, flags)) {
            const auto selectedAccounts = getSelectedAccountsInfo();

            if (selectedAccounts.empty()) {
                ImGui::Text("Status: %s", ButtonRightStatus::Get().c_str());
            } else {
                renderSelectedAccountsStatus(selectedAccounts);
            }
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
    }

}

bool RenderUI() {
    const bool exitFromMenu = RenderMainMenu();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    constexpr ImGuiWindowFlags mainFlags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::Begin("MainAppArea", nullptr, mainFlags);

    renderTabBar();

    renderStatusBar(viewport);

    ImGui::End();

    ModalPopup::Render();
	float deltaTime = ImGui::GetIO().DeltaTime;
	UpdateNotification::Update(deltaTime);
	UpdateNotification::Render();
	ProgressOverlay::Render(deltaTime);

    return exitFromMenu;
}