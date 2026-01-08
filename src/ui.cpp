#include "../ui.h"

#include <imgui.h>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

#include "accounts/accounts.h"
#include "components/components.h"
#include "friends/friends.h"
#include "games/games.h"
#include "history/history.h"
#include "settings/settings.h"
#include "network/roblox.h"
#include "core/status.h"
#include "ui/modal_popup.h"
#include "ui/confirm.h"
#include "avatar/inventory.h"

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
            const auto it = std::ranges::find_if(g_accounts,
                [id](const auto& acc) { return acc.id == id; });

            if (it == g_accounts.end()) continue;

            const auto& label = it->displayName.empty() ? it->username : it->displayName;
            result.push_back({label, getStatusColor(it->status)});
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
                ImGui::Text("Status: %s", Status::Get().c_str());
            } else {
                renderSelectedAccountsStatus(selectedAccounts);
            }
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
    }

}

char join_value_buf[JOIN_VALUE_BUF_SIZE] = "";
char join_jobid_buf[JOIN_JOBID_BUF_SIZE] = "";
int join_type_combo_index = 0;
int g_activeTab = Tab_Accounts;

uint64_t g_targetPlaceId_ServersTab = 0;
uint64_t g_targetUniverseId_ServersTab = 0;

#include "utils/system/update.h"

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
    ConfirmPopup::Render();
	float deltaTime = ImGui::GetIO().DeltaTime;
	UpdateNotification::Update(deltaTime);
	UpdateNotification::Render();

    return exitFromMenu;
}