#define _CRT_SECURE_NO_WARNINGS
#include <unordered_map>
#include <unordered_set>
#include "games_utils.h"
#include "games.h"
#include <imgui.h>
#include <vector>
#include <string>
#include <algorithm>
#include <thread>
#include <utility>

#include "../components.h"
#include "system/launcher.hpp"
#include "network/roblox.h"
#include "core/status.h"
#include "ui/webview.hpp"
#include "ui/modal_popup.h"
#include "../../ui.h"
#include "../servers/servers_utils.h"
#include "../accounts/accounts_join_ui.h"

using namespace ImGui;
using namespace std;

static char searchBuffer[64] = "";
static int selectedIndex = -1;
static vector<GameInfo> gamesList;
static vector<GameInfo> originalGamesList;
static unordered_map<uint64_t, Roblox::GameDetail> gameDetailCache;

static unordered_set<uint64_t> favoriteGameIds;
static auto ICON_OPEN_LINK = "\xEF\x8A\xBB ";
static auto ICON_JOIN = "\xEF\x8B\xB6 ";
static auto ICON_LAUNCH = "\xEF\x84\xB5 ";
static auto ICON_SERVER = "\xEF\x88\xB3 ";
static vector<GameInfo> favoriteGamesList;
static bool hasLoadedFavorites = false;
static char renameBuffer[128] = "";
static uint64_t renamingUniverseId = 0;

enum class GameSortMode {
    Relevance = 0,
    PlayersDesc,
    PlayersAsc,
    NameAsc,
    NameDesc
};

static GameSortMode currentSortMode = GameSortMode::Relevance;
static int sortComboIndex = 0;

static void SortGamesList();

static void RenderGameSearch();

static void RenderFavoritesList(float listWidth, float availableHeight);

static void RenderSearchResultsList(float listWidth, float availableHeight);

static void RenderGameDetailsPanel(float panelWidth, float availableHeight);

static void SortGamesList() {
    gamesList = originalGamesList;

    switch (currentSortMode) {
        case GameSortMode::PlayersDesc:
            sort(gamesList.begin(), gamesList.end(), [](const GameInfo &a, const GameInfo &b) {
                return a.playerCount > b.playerCount;
            });
            break;
        case GameSortMode::PlayersAsc:
            sort(gamesList.begin(), gamesList.end(), [](const GameInfo &a, const GameInfo &b) {
                return a.playerCount < b.playerCount;
            });
            break;
        case GameSortMode::NameAsc:
            sort(gamesList.begin(), gamesList.end(), [](const GameInfo &a, const GameInfo &b) {
                return a.name < b.name;
            });
            break;
        case GameSortMode::NameDesc:
            sort(gamesList.begin(), gamesList.end(), [](const GameInfo &a, const GameInfo &b) {
                return a.name > b.name;
            });
            break;
        case GameSortMode::Relevance:
        default:
            break;
    }
}

static void RenderGameSearch() {
    ImGuiStyle &style = GetStyle();
    const char *sortOptions[] = {
        "Relevance",
        "Players (Asc)",
        "Players (Desc)",
        "A-Z",
        "Z-A"
    };

    float searchButtonWidth = CalcTextSize(" Search  \xEF\x80\x82 ").x + style.FramePadding.x * 2.0f;

    float comboWidth = CalcTextSize("Players (Low-High)").x + style.FramePadding.x * 4.0f;
    float inputWidth = GetContentRegionAvail().x - searchButtonWidth - comboWidth - style.ItemSpacing.x * 2;
    if (inputWidth < 100.0f)
        inputWidth = 100.0f;
    PushItemWidth(inputWidth);
    InputTextWithHint("##game_search", "Search games", searchBuffer, sizeof(searchBuffer));
    PopItemWidth();
    SameLine(0, style.ItemSpacing.x);
    if (Button(" Search  \xEF\x80\x82 ", ImVec2(searchButtonWidth, 0)) && searchBuffer[0] != '\0') {
        selectedIndex = -1;
        originalGamesList = Roblox::searchGames(searchBuffer);
        erase_if(originalGamesList, [&](const GameInfo &g) {
            return favoriteGameIds.contains(g.universeId);
        });
        SortGamesList();
        gameDetailCache.clear();
    }
    SameLine(0, style.ItemSpacing.x);
    PushItemWidth(comboWidth);
    if (Combo(" Sort By", &sortComboIndex, sortOptions, IM_ARRAYSIZE(sortOptions))) {
        currentSortMode = static_cast<GameSortMode>(sortComboIndex);
        SortGamesList();
    }
    PopItemWidth();
}

static void RenderFavoritesList(float listWidth, float availableHeight) {
    if (!favoriteGamesList.empty()) {
        for (int index = 0; index < static_cast<int>(favoriteGamesList.size()); ++index) {
            const auto &game = favoriteGamesList[index];
            if (searchBuffer[0] != '\0' && !containsCI(game.name, searchBuffer))
                continue;
            PushID(("fav" + to_string(game.universeId)).c_str());
            TextUnformatted("\xEF\x80\x85");
            SameLine();
            if (Selectable(game.name.c_str(), selectedIndex == -1000 - index)) {
                selectedIndex = -1000 - index;
            }

            if (BeginPopupContextItem("FavoriteContext")) {
                if (MenuItem("Fill Join Options")) {
                    FillJoinOptions(game.placeId, "");
                }
                if (MenuItem("Copy Place ID")) {
                    SetClipboardText(to_string(game.placeId).c_str());
                }
                if (MenuItem("Copy Universe ID")) {
                    SetClipboardText(to_string(game.universeId).c_str());
                }
                
                if (BeginMenu("Rename")) {
                    if (renamingUniverseId != game.universeId) {
                        strncpy(renameBuffer, game.name.c_str(), sizeof(renameBuffer) - 1);
                        renameBuffer[sizeof(renameBuffer) - 1] = '\0';
                        renamingUniverseId = game.universeId;
                    }

                    ImGuiStyle &style = GetStyle();
                    float saveWidth = CalcTextSize("Save##RenameFavorite").x + style.FramePadding.x * 2.0f;
                    float cancelWidth = CalcTextSize("Cancel##RenameFavorite").x + style.FramePadding.x * 2.0f;
                    PushItemWidth(GetContentRegionAvail().x);
                    InputText("##RenameFavorite", renameBuffer, sizeof(renameBuffer));
                    PopItemWidth();

                    if (Button("Save##RenameFavorite", ImVec2(saveWidth, 0))) {
                        if (renamingUniverseId == game.universeId) {
                            favoriteGamesList[index].name = renameBuffer;
                            for (auto &f: g_favorites) {
                                if (f.universeId == game.universeId) {
                                    f.name = renameBuffer;
                                    break;
                                }
                            }
                            Data::SaveFavorites();
                        }
                        renamingUniverseId = 0;
                        CloseCurrentPopup();
                    }
                    SameLine(0, style.ItemSpacing.x);
                    if (Button("Cancel##RenameFavorite", ImVec2(cancelWidth, 0))) {
                        renamingUniverseId = 0;
                        CloseCurrentPopup();
                    }
                    ImGui::EndMenu();
                }
                Separator();
                PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
                if (MenuItem("Unfavorite")) {
                    uint64_t universeIdToRemove = game.universeId;
                    favoriteGameIds.erase(universeIdToRemove);
                    erase_if(favoriteGamesList,
                             [&](const GameInfo &gameInfo) {
                                 return gameInfo.universeId == universeIdToRemove;
                             });

                    if (selectedIndex == -1000 - index)
                        selectedIndex = -1;

                    erase_if(g_favorites,
                             [&](const FavoriteGame &favoriteGame) {
                                 return favoriteGame.universeId == universeIdToRemove;
                             });
                    Data::SaveFavorites();
                    CloseCurrentPopup();
                }
                PopStyleColor();
                EndPopup();
            }
            PopID();
        }
    }
}

static void RenderSearchResultsList(float listWidth, float availableHeight) {
    for (int index = 0; index < static_cast<int>(gamesList.size()); ++index) {
        const auto &game = gamesList[index];
        if (favoriteGameIds.contains(game.universeId))
            continue;
        PushID(static_cast<int>(game.universeId));

        if (Selectable(game.name.c_str(), selectedIndex == index)) {
            selectedIndex = index;
        }

        if (IsItemHovered())
            SetTooltip("Players: %s", formatWithCommas(game.playerCount).c_str());

        if (BeginPopupContextItem("GameContext")) {
            if (MenuItem("Copy Place ID"))
                SetClipboardText(to_string(game.placeId).c_str());
            if (MenuItem("Copy Universe ID"))
                SetClipboardText(to_string(game.universeId).c_str());
            if (MenuItem("Favorite") && !favoriteGameIds.contains(game.universeId)) {
                favoriteGameIds.insert(game.universeId);
                GameInfo favoriteGameInfo = game;
                favoriteGamesList.insert(favoriteGamesList.begin(), favoriteGameInfo);

                FavoriteGame favoriteGameData{game.name, game.universeId, game.placeId};
                g_favorites.push_back(favoriteGameData);
                Data::SaveFavorites();
                CloseCurrentPopup();
            }
            EndPopup();
        }
        PopID();
    }
}

void RenderGamesTab() {
    if (!hasLoadedFavorites) {
        Data::LoadFavorites();
        for (const auto &favoriteData: g_favorites) {
            favoriteGameIds.insert(favoriteData.universeId);
            GameInfo favoriteGameInfo{};
            favoriteGameInfo.name = favoriteData.name;
            favoriteGameInfo.placeId = favoriteData.placeId;
            favoriteGameInfo.universeId = favoriteData.universeId;
            favoriteGameInfo.playerCount = 0;
            favoriteGamesList.push_back(favoriteGameInfo);
        }
        hasLoadedFavorites = true;
    }

    RenderGameSearch();

    constexpr float GamesListWidth = 300.f;
    float availableHeight = GetContentRegionAvail().y;
    float availableWidth = GetContentRegionAvail().x;

    BeginChild("##GamesList", ImVec2(GamesListWidth, availableHeight), true);
    RenderFavoritesList(GamesListWidth, availableHeight);
    if (!favoriteGamesList.empty() && !gamesList.empty() && any_of(gamesList.begin(), gamesList.end(),
                                                                   [&](const GameInfo &gameInfo) {
                                                                       return !favoriteGameIds.contains(
                                                                           gameInfo.universeId);
                                                                   })) {
        Separator();
    }
    RenderSearchResultsList(GamesListWidth, availableHeight);
    EndChild();
    SameLine();

    RenderGameDetailsPanel(availableWidth - GamesListWidth - GetStyle().ItemSpacing.x, availableHeight);
}

static void RenderGameDetailsPanel(float panelWidth, float availableHeight) {
    float desiredTextIndent = 8.0f;

    PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    BeginChild("##GameDetails", ImVec2(panelWidth, availableHeight), true);
    PopStyleVar();

    const GameInfo *currentGameInfo = nullptr;
    uint64_t currentUniverseId = 0;

    if (selectedIndex <= -1000) {
        int favoriteIndex = -1000 - selectedIndex;
        if (favoriteIndex >= 0 && favoriteIndex < static_cast<int>(favoriteGamesList.size())) {
            currentGameInfo = &favoriteGamesList[favoriteIndex];
            currentUniverseId = currentGameInfo->universeId;
        }
    } else if (selectedIndex >= 0 && selectedIndex < static_cast<int>(gamesList.size())) {
        currentGameInfo = &gamesList[selectedIndex];
        currentUniverseId = currentGameInfo->universeId;
    }

    if (currentGameInfo) {
        const GameInfo &gameInfo = *currentGameInfo;
        Roblox::GameDetail detailInfo;
        auto cacheIterator = gameDetailCache.find(currentUniverseId);
        if (cacheIterator == gameDetailCache.end()) {
            if (currentUniverseId != 0) {
                detailInfo = Roblox::getGameDetail(currentUniverseId);
                gameDetailCache[currentUniverseId] = detailInfo;
            }
        } else {
            detailInfo = cacheIterator->second;
        }

        int serverCount = detailInfo.maxPlayers > 0
                              ? static_cast<int>(
                                  ceil(static_cast<double>(gameInfo.playerCount) / detailInfo.maxPlayers))
                              : 0;

        ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_SizingFixedFit;

        PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 4.0f));
        if (BeginTable("GameInfoTable", 2, tableFlags)) {
            TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, 140.f);
            TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

            auto addRow = [&](const char *label, const string &valueString,
                              const ImVec4 *color = nullptr) {
                TableNextRow();
                TableSetColumnIndex(0);
                Indent(desiredTextIndent);
                Spacing();
                TextUnformatted(label);
                Spacing();
                Unindent(desiredTextIndent);

                TableSetColumnIndex(1);
                Indent(desiredTextIndent);
                Spacing();
                PushID(label);
                if (color)
                    PushStyleColor(ImGuiCol_Text, *color);
                TextWrapped("%s", valueString.c_str());
                if (color)
                    PopStyleColor();
                if (BeginPopupContextItem("CopyGameValue")) {
                    if (MenuItem("Copy")) {
                        SetClipboardText(valueString.c_str());
                    }
                    EndPopup();
                }
                PopID();
                Spacing();
                Unindent(desiredTextIndent);
            };

            string displayName = detailInfo.name.empty() ? gameInfo.name : detailInfo.name;
            addRow("Name:", displayName);
            addRow("Place ID:", to_string(gameInfo.placeId));
            addRow("Universe ID:", to_string(gameInfo.universeId));
            const ImVec4 verifiedColor = ImVec4(0.031f, 0.392f, 0.988f, 1.f); // #0864fc
            addRow("Creator:",
                   detailInfo.creatorName +
                   string(detailInfo.creatorVerified ? " \xEF\x80\x8C" : ""),
                   detailInfo.creatorVerified ? &verifiedColor : nullptr);
            addRow("Players:", formatWithCommas(gameInfo.playerCount));
            addRow("Max Players:", formatWithCommas(detailInfo.maxPlayers));
            addRow("Visits:", formatWithCommas(detailInfo.visits));
            addRow("Genre:", detailInfo.genre);
            if (serverCount > 0)
                addRow("Est. Servers:", formatWithCommas(serverCount));

            TableNextRow();
            TableSetColumnIndex(0);
            Indent(desiredTextIndent);
            Spacing();
            TextUnformatted("Description:");
            Spacing();
            Unindent(desiredTextIndent);

            TableSetColumnIndex(1);
            Indent(desiredTextIndent);
            Spacing();

            ImGuiStyle &style = GetStyle();

            float spaceForBottomSpacingInCell = style.ItemSpacing.y;

            float spaceForSeparator = style.ItemSpacing.y;

            float spaceForButtons = GetFrameHeightWithSpacing();

            float reservedHeightBelowDescContent = spaceForBottomSpacingInCell + spaceForSeparator + spaceForButtons;

            float availableHeightForDescAndBelow = GetContentRegionAvail().y;

            float descChildHeight = availableHeightForDescAndBelow - reservedHeightBelowDescContent;

            float minDescHeight = GetTextLineHeightWithSpacing() * 3.0f;
            if (descChildHeight < minDescHeight) {
                descChildHeight = minDescHeight;
            }

            const string descStr = detailInfo.description;
            PushID("GameDesc");
            BeginChild("##DescScroll", ImVec2(0, descChildHeight - 4), false,
                       ImGuiWindowFlags_HorizontalScrollbar);
            TextWrapped("%s", descStr.c_str());
            if (BeginPopupContextItem("CopyGameDesc")) {
                if (MenuItem("Copy")) {
                    SetClipboardText(descStr.c_str());
                }
                EndPopup();
            }
            EndChild();
            PopID();

            Spacing();
            Unindent(desiredTextIndent);

            EndTable();
        }
        PopStyleVar();

        Separator();

        Indent(desiredTextIndent / 2);
        if (Button((string(ICON_LAUNCH) + " Launch Game").c_str())) {
            if (!g_selectedAccountIds.empty()) {
                vector<pair<int, string> > accounts;
                for (int id: g_selectedAccountIds) {
                    auto it = find_if(g_accounts.begin(), g_accounts.end(),
                                      [&](const AccountData &a) { return a.id == id; });
                    if (it != g_accounts.end() && it->status != "Banned" && it->status != "Warned" && it->status != "Terminated")
                        accounts.emplace_back(it->id, it->cookie);
                }
                if (!accounts.empty()) {
                    thread([placeId = gameInfo.placeId, accounts]() {
                                launchRobloxSequential(placeId, "", accounts);
                            })
                            .detach();
                } else {
                    Status::Error("Selected account not found to launch game.");
                }
            } else {
                Status::Error("No account selected to launch game.");
                ModalPopup::Add("Select an account first.");
            }
        }
        SameLine();
        if (Button((string(ICON_SERVER) + " View Servers").c_str())) {
            g_activeTab = Tab_Servers;
            g_targetPlaceId_ServersTab = gameInfo.placeId;
            g_targetUniverseId_ServersTab = gameInfo.universeId;
        }
        SameLine();
        bool openPageBtn = Button((string(ICON_OPEN_LINK) + " Open Page").c_str());
        if (openPageBtn)
            OpenPopup("GamePageMenu");
        OpenPopupOnItemClick("GamePageMenu");
        if (BeginPopup("GamePageMenu")) {
            // Get cookie from primary selected account (first in selected accounts list)
            string primaryCookie;
            if (!g_selectedAccountIds.empty()) {
                auto primaryId = *g_selectedAccountIds.begin();
                auto it = find_if(g_accounts.begin(), g_accounts.end(),
                    [primaryId](const AccountData &a) { return a.id == primaryId; });
                if (it != g_accounts.end()) {
                    primaryCookie = it->cookie;
                }
            }

            if (MenuItem("Roblox Page"))
                LaunchWebview("https://www.roblox.com/games/" + to_string(gameInfo.placeId), "Game Page", primaryCookie);
            if (MenuItem("Rolimons"))
                LaunchWebview("https://www.rolimons.com/game/" + to_string(gameInfo.placeId) + "/", "Rolimons", primaryCookie);
            if (MenuItem("RoMonitor"))
                LaunchWebview("https://romonitorstats.com/experience/" + to_string(gameInfo.placeId) + "/", "RoMonitor Stats", primaryCookie);
            EndPopup();
        }
        Unindent(desiredTextIndent / 2);
    } else {
        Indent(desiredTextIndent);
        Spacing();
        TextWrapped("Select a game from the list to see details or add a favorite.");
        Unindent(desiredTextIndent);
    }

    EndChild();
}
