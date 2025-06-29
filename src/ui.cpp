#include "../ui.h"

#include <imgui.h>
#include <string>

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

using namespace ImGui;

struct TabInfo {
    const char *title;
    Tab tab_id;

    void (*render_function)();
};

static const TabInfo tabs[] = {
    {"\xEF\x80\x87  Accounts", Tab_Accounts, RenderFullAccountsTabContent},
    {"\xEF\x83\x80  Friends", Tab_Friends, RenderFriendsTab},
    {"\xEF\x84\x9B  Games", Tab_Games, RenderGamesTab},
    {"\xEF\x88\xB3  Servers", Tab_Servers, RenderServersTab},
    {"\xEF\x85\x9C  History", Tab_History, RenderHistoryTab},
    {"\xEF\x8A\xA8  Console", Tab_Console, Console::RenderConsoleTab},
    {"\xEF\x80\x93  Settings", Tab_Settings, RenderSettingsTab}
};

char join_value_buf[JOIN_VALUE_BUF_SIZE] = "";
char join_jobid_buf[JOIN_JOBID_BUF_SIZE] = "";
int join_type_combo_index = 0;
int g_activeTab = Tab_Accounts;

// Define the new global variables here
uint64_t g_targetPlaceId_ServersTab = 0;
uint64_t g_targetUniverseId_ServersTab = 0;

bool RenderUI() {
    bool exit_from_menu = RenderMainMenu();
    bool exit_from_content = false;

    const ImGuiViewport *vp = GetMainViewport();
    SetNextWindowPos(vp->WorkPos);
    SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags mainFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                 ImGuiWindowFlags_NoResize;
    Begin("MainAppArea", nullptr, mainFlags);

    ImGuiStyle &style = GetStyle();
    style.FrameRounding = 2.5f;
    style.ChildRounding = 2.5f;
    PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x + 2.0f, style.FramePadding.y + 2.0f));
    if (BeginTabBar("MainTabBar", ImGuiTabBarFlags_Reorderable)) {
        for (int i = 0; i < IM_ARRAYSIZE(tabs); ++i) {
            const auto &tab_info = tabs[i];
            ImGuiTabItemFlags flags = (g_activeTab == tab_info.tab_id)
                                          ? ImGuiTabItemFlags_SetSelected
                                          : ImGuiTabItemFlags_None;
            bool opened = BeginTabItem(tab_info.title, nullptr, flags);

            if (IsItemClicked(ImGuiMouseButton_Left))
                g_activeTab = tab_info.tab_id;

            if (opened) {
                tab_info.render_function();
                EndTabItem();
            }
        }
        EndTabBar();
    }
    PopStyleVar(); {
        ImVec2 pos = ImVec2(vp->WorkPos.x + vp->WorkSize.x,
                            vp->WorkPos.y + vp->WorkSize.y);
        SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(1, 1));

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing;
        PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
        PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

        if (Begin("StatusBar", nullptr, flags)) {
            string selectedNames;
            bool first = true;
            for (int id: g_selectedAccountIds) {
                auto it = find_if(g_accounts.begin(), g_accounts.end(),
                                  [&](const AccountData &a) { return a.id == id; });
                if (it == g_accounts.end())
                    continue;
                if (!first)
                    selectedNames += ", ";
                string name = it->displayName.empty() ? it->username : it->displayName;
                if (first)
                    name += "*";
                selectedNames += name;
                first = false;
            }

            if (selectedNames.empty())
                Text("Status: %s", Status::Get().c_str());
            else
                Text("Selected: %s", selectedNames.c_str());
        }
        End();
        PopStyleVar(2);
    }

    End();

    ModalPopup::Render();
    ConfirmPopup::Render();

    return exit_from_menu || exit_from_content;
}
