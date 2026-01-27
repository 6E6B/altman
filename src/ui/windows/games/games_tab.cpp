#include <algorithm>
#include <format>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <imgui.h>

#include "games.h"
#include "games_utils.h"
#include "network/roblox/auth.h"
#include "network/roblox/common.h"
#include "network/roblox/games.h"
#include "network/roblox/session.h"
#include "network/roblox/social.h"
#include "system/roblox_launcher.h"
#include "ui/ui.h"
#include "ui/webview/webview.h"
#include "ui/widgets/bottom_right_status.h"
#include "ui/widgets/context_menus.h"
#include "ui/widgets/modal_popup.h"
#include "ui/windows/accounts/accounts_join_ui.h"
#include "ui/windows/components.h"
#include "ui/windows/servers/servers_utils.h"
#include "utils/account_utils.h"
#include "utils/time_utils.h"

namespace {
    constexpr size_t SEARCH_BUFFER_SIZE = 64;
    constexpr size_t RENAME_BUFFER_SIZE = 128;
    constexpr int INVALID_INDEX = -1;
    constexpr int FAVORITE_INDEX_OFFSET = -1000;

    constexpr const char *ICON_OPEN_LINK = "\xEF\x8A\xBB ";
    constexpr const char *ICON_JOIN = "\xEF\x8B\xB6 ";
    constexpr const char *ICON_LAUNCH = "\xEF\x84\xB5 ";
    constexpr const char *ICON_SERVER = "\xEF\x88\xB3 ";

    constexpr ImVec4 VERIFIED_COLOR {0.031f, 0.392f, 0.988f, 1.f};
    constexpr ImVec4 ERROR_COLOR {1.f, 0.4f, 0.4f, 1.f};

    char searchBuffer[SEARCH_BUFFER_SIZE] = "";
    char renameBuffer[RENAME_BUFFER_SIZE] = "";

    int selectedIndex = INVALID_INDEX;
    uint64_t renamingUniverseId = 0;
    bool hasLoadedFavorites = false;

    std::vector<GameInfo> gamesList;
    std::vector<GameInfo> originalGamesList;
    std::vector<GameInfo> favoriteGamesList;

    std::unordered_map<uint64_t, Roblox::GameDetail> gameDetailCache;
    std::unordered_set<uint64_t> favoriteGameIds;

    enum class GameSortMode {
        Relevance = 0,
        PlayersDesc,
        PlayersAsc,
        NameAsc,
        NameDesc
    };

    GameSortMode currentSortMode = GameSortMode::Relevance;
    int sortComboIndex = 0;

    template<typename Container, typename Pred> void EraseIf(Container &container, Pred predicate) {
        container.erase(std::remove_if(container.begin(), container.end(), predicate), container.end());
    }

    void SortGamesList() {
        gamesList = originalGamesList;

        switch (currentSortMode) {
            case GameSortMode::PlayersDesc:
                std::sort(gamesList.begin(), gamesList.end(), [](const GameInfo &a, const GameInfo &b) {
                    return a.playerCount > b.playerCount;
                });
                break;
            case GameSortMode::PlayersAsc:
                std::sort(gamesList.begin(), gamesList.end(), [](const GameInfo &a, const GameInfo &b) {
                    return a.playerCount < b.playerCount;
                });
                break;
            case GameSortMode::NameAsc:
                std::sort(gamesList.begin(), gamesList.end(), [](const GameInfo &a, const GameInfo &b) {
                    return a.name < b.name;
                });
                break;
            case GameSortMode::NameDesc:
                std::sort(gamesList.begin(), gamesList.end(), [](const GameInfo &a, const GameInfo &b) {
                    return a.name > b.name;
                });
                break;
            case GameSortMode::Relevance:
            default:
                break;
        }
    }

    void ClearSearchState() {
        searchBuffer[0] = '\0';
        selectedIndex = INVALID_INDEX;
        originalGamesList.clear();
        gamesList.clear();
        gameDetailCache.clear();
    }

    void PerformSearch() {
        if (searchBuffer[0] == '\0') {
            return;
        }

        selectedIndex = INVALID_INDEX;
        originalGamesList = Roblox::searchGames(searchBuffer);

        EraseIf(originalGamesList, [](const GameInfo &game) {
            return favoriteGameIds.contains(game.universeId);
        });

        SortGamesList();
        gameDetailCache.clear();
    }

    void LaunchGameWithAccounts(uint64_t placeId) {
        launchWithSelectedAccounts(LaunchParams::standard(placeId));
    }

    void RenderStandardGameMenu(uint64_t placeId, uint64_t universeId) {
        StandardJoinMenuParams menu {};
        menu.placeId = placeId;
        menu.universeId = universeId;
        menu.jobId = "";
        menu.onLaunchGame = [placeId]() {
            LaunchGameWithAccounts(placeId);
        };
        menu.onFillGame = [placeId]() {
            FillJoinOptions(placeId, "");
        };
        RenderStandardJoinMenu(menu);
    }

    void RenderRenameMenu(const GameInfo &game, int index) {
        if (!ImGui::BeginMenu("Rename")) {
            return;
        }

        if (renamingUniverseId != game.universeId) {
            std::strncpy(renameBuffer, game.name.c_str(), RENAME_BUFFER_SIZE - 1);
            renameBuffer[RENAME_BUFFER_SIZE - 1] = '\0';
            renamingUniverseId = game.universeId;
        }

        const ImGuiStyle &style = ImGui::GetStyle();
        const float saveWidth = ImGui::CalcTextSize("Save##RenameFavorite").x + style.FramePadding.x * 2.0f;
        const float cancelWidth = ImGui::CalcTextSize("Cancel##RenameFavorite").x + style.FramePadding.x * 2.0f;

        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputText("##RenameFavorite", renameBuffer, RENAME_BUFFER_SIZE);
        ImGui::PopItemWidth();

        if (ImGui::Button("Save##RenameFavorite", ImVec2(saveWidth, 0))) {
            if (renamingUniverseId == game.universeId) {
                favoriteGamesList[index].name = renameBuffer;

                for (auto &favorite: g_favorites) {
                    if (favorite.universeId == game.universeId) {
                        favorite.name = renameBuffer;
                        break;
                    }
                }

                Data::SaveFavorites();
            }
            renamingUniverseId = 0;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine(0, style.ItemSpacing.x);

        if (ImGui::Button("Cancel##RenameFavorite", ImVec2(cancelWidth, 0))) {
            renamingUniverseId = 0;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndMenu();
    }

    void UnfavoriteGame(uint64_t universeId, int index) {
        favoriteGameIds.erase(universeId);

        EraseIf(favoriteGamesList, [universeId](const GameInfo &game) {
            return game.universeId == universeId;
        });

        if (selectedIndex == FAVORITE_INDEX_OFFSET - index) {
            selectedIndex = INVALID_INDEX;
        }

        EraseIf(g_favorites, [universeId](const FavoriteGame &favorite) {
            return favorite.universeId == universeId;
        });

        Data::SaveFavorites();
    }

    void FavoriteGameFunc(const GameInfo &game) {
        if (favoriteGameIds.contains(game.universeId)) {
            return;
        }

        favoriteGameIds.insert(game.universeId);
        favoriteGamesList.insert(favoriteGamesList.begin(), game);

        FavoriteGame favoriteData {game.name, game.universeId, game.placeId};
        g_favorites.push_back(favoriteData);
        Data::SaveFavorites();
    }

    void RenderGameSearch() {
        const ImGuiStyle &style = ImGui::GetStyle();

        constexpr const char *sortOptions[] = {"Relevance", "Players (Asc)", "Players (Desc)", "A-Z", "Z-A"};

        const float searchButtonWidth = ImGui::CalcTextSize(" \xEF\x80\x82  Search ").x + style.FramePadding.x * 2.0f;
        const float clearButtonWidth = ImGui::CalcTextSize(" \xEF\x87\xB8  Clear ").x + style.FramePadding.x * 2.0f;
        const float comboWidth = ImGui::CalcTextSize("Players (Low-High)").x + style.FramePadding.x * 4.0f;
        const float minFieldWidth = ImGui::GetFontSize() * 6.25f;

        float inputWidth = ImGui::GetContentRegionAvail().x - searchButtonWidth - clearButtonWidth - comboWidth
                           - style.ItemSpacing.x * 3;
        inputWidth = std::max(inputWidth, minFieldWidth);

        ImGui::PushItemWidth(inputWidth);
        ImGui::InputTextWithHint("##game_search", "Search games", searchBuffer, SEARCH_BUFFER_SIZE);
        ImGui::PopItemWidth();

        ImGui::SameLine(0, style.ItemSpacing.x);

        if (ImGui::Button(" \xEF\x80\x82  Search ", ImVec2(searchButtonWidth, 0))) {
            PerformSearch();
        }

        ImGui::SameLine(0, style.ItemSpacing.x);

        if (ImGui::Button(" \xEF\x87\xB8  Clear ", ImVec2(clearButtonWidth, 0))) {
            ClearSearchState();
        }

        ImGui::SameLine(0, style.ItemSpacing.x);

        ImGui::PushItemWidth(comboWidth);
        if (ImGui::Combo(" Sort By", &sortComboIndex, sortOptions, IM_ARRAYSIZE(sortOptions))) {
            currentSortMode = static_cast<GameSortMode>(sortComboIndex);
            SortGamesList();
        }
        ImGui::PopItemWidth();
    }

    void RenderFavoritesList(float listWidth, float availableHeight) {
        for (int index = 0; index < static_cast<int>(favoriteGamesList.size()); ++index) {
            const auto &game = favoriteGamesList[index];

            if (searchBuffer[0] != '\0' && !containsCI(game.name, searchBuffer)) {
                continue;
            }

            ImGui::PushID(std::format("fav{}", game.universeId).c_str());

            ImGui::TextUnformatted("\xEF\x80\x85");
            ImGui::SameLine();

            if (ImGui::Selectable(game.name.c_str(), selectedIndex == FAVORITE_INDEX_OFFSET - index)) {
                selectedIndex = FAVORITE_INDEX_OFFSET - index;
            }

            if (ImGui::BeginPopupContextItem("FavoriteContext")) {
                RenderStandardGameMenu(game.placeId, game.universeId);
                RenderRenameMenu(game, index);

                ImGui::Separator();

                ImGui::PushStyleColor(ImGuiCol_Text, ERROR_COLOR);
                if (ImGui::MenuItem("Unfavorite")) {
                    UnfavoriteGame(game.universeId, index);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopStyleColor();

                ImGui::EndPopup();
            }

            ImGui::PopID();
        }
    }

    void RenderSearchResultsList(float listWidth, float availableHeight) {
        for (int index = 0; index < static_cast<int>(gamesList.size()); ++index) {
            const auto &game = gamesList[index];

            if (favoriteGameIds.contains(game.universeId)) {
                continue;
            }

            ImGui::PushID(static_cast<int>(game.universeId));

            if (ImGui::Selectable(game.name.c_str(), selectedIndex == index)) {
                selectedIndex = index;
            }

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Players: %s", formatWithCommas(game.playerCount).c_str());
            }

            if (ImGui::BeginPopupContextItem("GameContext")) {
                RenderStandardGameMenu(game.placeId, game.universeId);

                if (ImGui::MenuItem("Favorite")) {
                    FavoriteGameFunc(game);
                    ImGui::CloseCurrentPopup();
                }

                ImGui::EndPopup();
            }

            ImGui::PopID();
        }
    }

    void AddGameInfoRow(const char *label, const std::string &value, const ImVec4 *color = nullptr) {
        constexpr float TEXT_INDENT = 8.0f;

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Indent(TEXT_INDENT);
        ImGui::Spacing();
        ImGui::TextUnformatted(label);
        ImGui::Spacing();
        ImGui::Unindent(TEXT_INDENT);

        ImGui::TableSetColumnIndex(1);
        ImGui::Indent(TEXT_INDENT);
        ImGui::Spacing();
        ImGui::PushID(label);

        if (color) {
            ImGui::PushStyleColor(ImGuiCol_Text, *color);
        }

        ImGui::TextWrapped("%s", value.c_str());

        if (color) {
            ImGui::PopStyleColor();
        }

        if (ImGui::BeginPopupContextItem("CopyGameValue")) {
            if (ImGui::MenuItem("Copy")) {
                ImGui::SetClipboardText(value.c_str());
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
        ImGui::Spacing();
        ImGui::Unindent(TEXT_INDENT);
    }

    float CalculateLabelColumnWidth() {
        const std::vector<const char *> labels
            = {"Name:",
               "Place ID:",
               "Universe ID:",
               "Creator:",
               "Creator ID:",
               "Creator Type:",
               "Players:",
               "Visits:",
               "Favorites:",
               "Max Players:",
               "Price:",
               "Created:",
               "Updated:",
               "Genre:",
               "Est. Servers:"};

        float maxWidth = ImGui::GetFontSize() * 8.75f;
        for (const char *label: labels) {
            maxWidth = std::max(maxWidth, ImGui::CalcTextSize(label).x);
        }

        return maxWidth + ImGui::GetFontSize() * 2.0f;
    }

    void RenderGameInfoTable(const GameInfo &gameInfo, const Roblox::GameDetail &detailInfo) {
        const int serverCount
            = detailInfo.maxPlayers > 0
                  ? static_cast<int>(std::ceil(static_cast<double>(gameInfo.playerCount) / detailInfo.maxPlayers))
                  : 0;

        constexpr ImGuiTableFlags tableFlags
            = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;

        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 4.0f));

        const float labelColumnWidth = CalculateLabelColumnWidth();

        if (ImGui::BeginTable("GameInfoTable", 2, tableFlags)) {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelColumnWidth);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

            const std::string displayName = detailInfo.name.empty() ? gameInfo.name : detailInfo.name;
            AddGameInfoRow("Name:", displayName);
            AddGameInfoRow("Place ID:", std::format("{}", gameInfo.placeId));
            AddGameInfoRow("Universe ID:", std::format("{}", gameInfo.universeId));

            const std::string creatorText = detailInfo.creatorVerified
                                                ? std::format("{} \xEF\x80\x8C", detailInfo.creatorName)
                                                : detailInfo.creatorName;
            AddGameInfoRow("Creator:", creatorText, detailInfo.creatorVerified ? &VERIFIED_COLOR : nullptr);

            AddGameInfoRow("Creator ID:", std::format("{}", detailInfo.creatorId));
            AddGameInfoRow("Creator Type:", detailInfo.creatorType.empty() ? "Unknown" : detailInfo.creatorType);

            const int playersNow = detailInfo.playing > 0 ? detailInfo.playing : gameInfo.playerCount;
            AddGameInfoRow("Players:", formatWithCommas(playersNow));
            AddGameInfoRow("Visits:", formatWithCommas(detailInfo.visits));
            AddGameInfoRow("Favorites:", formatWithCommas(detailInfo.favorites));
            AddGameInfoRow("Max Players:", formatWithCommas(detailInfo.maxPlayers));

            const std::string priceText
                = detailInfo.priceRobux >= 0 ? std::format("{} R$", formatWithCommas(detailInfo.priceRobux)) : "0 R$";
            AddGameInfoRow("Price:", priceText);

            AddGameInfoRow("Created:", formatAbsoluteWithRelativeFromIso(detailInfo.createdIso));
            AddGameInfoRow("Updated:", formatAbsoluteWithRelativeFromIso(detailInfo.updatedIso));

            std::string genreCombined = detailInfo.genre;
            if (!detailInfo.genreL1.empty()) {
                if (!genreCombined.empty()) {
                    genreCombined.append(", ");
                }
                genreCombined.append(detailInfo.genreL1);
            }
            if (!detailInfo.genreL2.empty()) {
                if (!genreCombined.empty()) {
                    genreCombined.append(", ");
                }
                genreCombined.append(detailInfo.genreL2);
            }
            AddGameInfoRow("Genre:", genreCombined);

            if (serverCount > 0) {
                AddGameInfoRow("Est. Servers:", formatWithCommas(serverCount));
            }

            constexpr float TEXT_INDENT = 8.0f;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Indent(TEXT_INDENT);
            ImGui::Spacing();
            ImGui::TextUnformatted("Description:");
            ImGui::Spacing();
            ImGui::Unindent(TEXT_INDENT);

            ImGui::TableSetColumnIndex(1);
            ImGui::Indent(TEXT_INDENT);
            ImGui::Spacing();

            const ImGuiStyle &style = ImGui::GetStyle();
            const float reservedHeight = style.ItemSpacing.y + style.ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
            const float availableHeight = ImGui::GetContentRegionAvail().y;
            const float minHeight = ImGui::GetTextLineHeightWithSpacing() * 3.0f;
            float descHeight = std::max(availableHeight - reservedHeight, minHeight);

            ImGui::PushID("GameDesc");
            ImGui::BeginChild("##DescScroll", ImVec2(0, descHeight - 4), false, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextWrapped("%s", detailInfo.description.c_str());

            if (ImGui::BeginPopupContextItem("CopyGameDesc")) {
                if (ImGui::MenuItem("Copy")) {
                    ImGui::SetClipboardText(detailInfo.description.c_str());
                }
                ImGui::EndPopup();
            }

            ImGui::EndChild();
            ImGui::PopID();

            ImGui::Spacing();
            ImGui::Unindent(TEXT_INDENT);

            ImGui::EndTable();
        }

        ImGui::PopStyleVar();
    }

    void RenderGameButtons(const GameInfo &gameInfo) {
        constexpr float BUTTON_INDENT = 4.0f;

        ImGui::Indent(BUTTON_INDENT);

        if (ImGui::Button(std::format("{}Launch Game", ICON_LAUNCH).c_str())) {
            launchWithSelectedAccounts(LaunchParams::standard(gameInfo.placeId));
        }

        ImGui::SameLine();

        if (ImGui::Button(std::format("{}View Servers", ICON_SERVER).c_str())) {
            g_activeTab = Tab_Servers;
            g_targetPlaceId_ServersTab = gameInfo.placeId;
            g_targetUniverseId_ServersTab = gameInfo.universeId;
        }

        ImGui::SameLine();

        if (ImGui::Button(std::format("{}Open Page", ICON_OPEN_LINK).c_str())) {
            ImGui::OpenPopup("GamePageMenu");
        }

        ImGui::OpenPopupOnItemClick("GamePageMenu");

        if (ImGui::BeginPopup("GamePageMenu")) {
            std::string primaryCookie;
            std::string primaryUserId;

            if (!g_selectedAccountIds.empty()) {
                if (const AccountData *acc = getAccountById(*g_selectedAccountIds.begin())) {
                    primaryCookie = acc->cookie;
                    primaryUserId = acc->userId;
                }
            }

            if (ImGui::MenuItem("Roblox Page")) {
                LaunchWebviewImpl(
                    std::format("https://www.roblox.com/games/{}", gameInfo.placeId),
                    "Game Page",
                    primaryCookie,
                    primaryUserId
                );
            }

            if (ImGui::MenuItem("Rolimons")) {
                LaunchWebviewImpl(
                    std::format("https://www.rolimons.com/game/{}/", gameInfo.placeId),
                    "Rolimons",
                    primaryCookie,
                    primaryUserId
                );
            }

            if (ImGui::MenuItem("RoMonitor")) {
                LaunchWebviewImpl(
                    std::format("https://romonitorstats.com/experience/{}/", gameInfo.placeId),
                    "RoMonitor Stats",
                    primaryCookie,
                    primaryUserId
                );
            }

            ImGui::EndPopup();
        }

        ImGui::Unindent(BUTTON_INDENT);
    }

    void RenderGameDetailsPanel(float panelWidth, float availableHeight) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("##GameDetails", ImVec2(panelWidth, availableHeight), true);
        ImGui::PopStyleVar();

        const GameInfo *currentGameInfo = nullptr;
        uint64_t currentUniverseId = 0;

        if (selectedIndex <= FAVORITE_INDEX_OFFSET) {
            const int favoriteIndex = FAVORITE_INDEX_OFFSET - selectedIndex;
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
            if (auto it = gameDetailCache.find(currentUniverseId); it != gameDetailCache.end()) {
                detailInfo = it->second;
            } else if (currentUniverseId != 0) {
                detailInfo = Roblox::getGameDetail(currentUniverseId);
                gameDetailCache[currentUniverseId] = detailInfo;
            }

            RenderGameInfoTable(gameInfo, detailInfo);
            ImGui::Separator();
            RenderGameButtons(gameInfo);
        } else {
            constexpr float TEXT_INDENT = 8.0f;
            ImGui::Indent(TEXT_INDENT);
            ImGui::Spacing();
            ImGui::TextWrapped("Select a game from the list to see details or add a favorite.");
            ImGui::Unindent(TEXT_INDENT);
        }

        ImGui::EndChild();
    }

    void LoadFavoritesOnce() {
        if (hasLoadedFavorites) {
            return;
        }

        Data::LoadFavorites();

        for (const auto &favoriteData: g_favorites) {
            favoriteGameIds.insert(favoriteData.universeId);

            GameInfo favoriteGameInfo {};
            favoriteGameInfo.name = favoriteData.name;
            favoriteGameInfo.placeId = favoriteData.placeId;
            favoriteGameInfo.universeId = favoriteData.universeId;
            favoriteGameInfo.playerCount = 0;

            favoriteGamesList.push_back(favoriteGameInfo);
        }

        hasLoadedFavorites = true;
    }

    bool ShouldShowSeparator() {
        if (favoriteGamesList.empty() || gamesList.empty()) {
            return false;
        }

        return std::any_of(gamesList.begin(), gamesList.end(), [](const GameInfo &game) {
            return !favoriteGameIds.contains(game.universeId);
        });
    }
} // namespace

void RenderGamesTab() {
    LoadFavoritesOnce();
    RenderGameSearch();

    const float availableHeight = ImGui::GetContentRegionAvail().y;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float fontSize = ImGui::GetFontSize();

    const float minSideWidth = fontSize * 14.0f;
    const float maxSideWidth = fontSize * 20.0f;
    float sideWidth = std::clamp(availableWidth * 0.28f, minSideWidth, maxSideWidth);

    ImGui::BeginChild("##GamesList", ImVec2(sideWidth, availableHeight), true);

    RenderFavoritesList(sideWidth, availableHeight);

    if (ShouldShowSeparator()) {
        ImGui::Separator();
    }

    RenderSearchResultsList(sideWidth, availableHeight);

    ImGui::EndChild();
    ImGui::SameLine();

    RenderGameDetailsPanel(availableWidth - sideWidth - ImGui::GetStyle().ItemSpacing.x, availableHeight);
}
