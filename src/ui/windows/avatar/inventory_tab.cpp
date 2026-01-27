#include "inventory.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <expected>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "components/data.h"
#include "network/roblox/common.h"
#include "ui/widgets/image.h"
#include "utils/thread_task.h"

namespace {
    constexpr float THUMB_ROUNDING = 6.0f;
    constexpr int MAX_CONCURRENT_THUMB_LOADS = 8;
    constexpr int BATCH_THUMBNAIL_SIZE = 50;
    constexpr float MIN_CELL_SIZE_MULTIPLIER = 6.25f;
    constexpr float MIN_FIELD_MULTIPLIER = 6.25f;
    constexpr float EQUIPPED_MIN_CELL_MULTIPLIER = 3.75f;
    constexpr size_t SEARCH_BUFFER_SIZE = 64;

    struct InventoryItem {
            uint64_t assetId {};
            std::string assetName;
    };

    struct CategoryInfo {
            std::string displayName;
            std::vector<std::pair<int, std::string>> assetTypes;
    };

    struct ThumbInfo {
            TextureHandle texture;
            int width {0};
            int height {0};
            bool loading {false};
            bool failed {false};
            [[nodiscard]] bool hasTexture() const {
                return static_cast<bool>(texture);
            }
    };

    struct AvatarState {
            TextureHandle texture;
            int imageWidth {0};
            int imageHeight {0};
            bool loading {false};
            bool failed {false};
            bool started {false};
            uint64_t loadedUserId {0};
            [[nodiscard]] bool hasTexture() const {
                return static_cast<bool>(texture);
            }
    };

    struct CategoryState {
            uint64_t userId {0};
            bool loading {false};
            bool failed {false};
            std::vector<CategoryInfo> categories;
            int selectedCategory {0};
    };

    struct InventoryState {
            std::unordered_map<int, std::vector<InventoryItem>> cachedInventories;
            int selectedAssetTypeIndex {0};
            bool loading {false};
            bool failed {false};
    };

    struct EquippedState {
            uint64_t userId {0};
            bool loading {false};
            bool failed {false};
            std::vector<uint64_t> assetIds;
    };

    struct BatchThumbState {
            std::vector<uint64_t> pendingAssetIds;
            bool batchLoading {false};
    };

    AvatarState g_avatarState;
    CategoryState g_categoryState;
    InventoryState g_inventoryState;
    EquippedState g_equippedState;
    BatchThumbState g_batchThumbState;

    std::unordered_map<uint64_t, ThumbInfo> g_thumbCache;
    uint64_t g_selectedAssetId {0};
    int g_activeThumbLoads {0};
    char g_searchBuffer[SEARCH_BUFFER_SIZE] = "";

    void ClearTextureCache() {
        g_thumbCache.clear();
    }

    void ResetAvatarTexture() {
        g_avatarState.texture.reset();
    }

    void ResetAllState() {
        g_categoryState = CategoryState {};
        g_inventoryState = InventoryState {};
        g_equippedState = EquippedState {};
        g_batchThumbState = BatchThumbState {};
        g_searchBuffer[0] = '\0';
        ClearTextureCache();
    }

    [[nodiscard]]
    std::expected<uint64_t, std::string> parseUserId(const std::string &str) {
        if (str.empty()) {
            return std::unexpected("Empty user ID");
        }

        char *end = nullptr;
        const uint64_t result = std::strtoull(str.c_str(), &end, 10);

        if (end == str.c_str() || *end != '\0') {
            return std::unexpected("Invalid user ID format");
        }

        return result;
    }

    [[nodiscard]]
    std::pair<uint64_t, std::string> GetCurrentUserInfo() {
        uint64_t userId = 0;
        std::string cookie;

        auto tryGetUserInfo = [&](int accountId) -> bool {
            if (const AccountData *acc = getAccountById(accountId)) {
                if (!acc->userId.empty()) {
                    auto result = parseUserId(acc->userId);
                    if (result) {
                        userId = *result;
                        cookie = acc->cookie;
                        return true;
                    }
                }
            }
            return false;
        };

        if (!g_selectedAccountIds.empty()) {
            tryGetUserInfo(*g_selectedAccountIds.begin());
        } else if (g_defaultAccountId != -1) {
            tryGetUserInfo(g_defaultAccountId);
        }

        return {userId, cookie};
    }

    [[nodiscard]]
    std::expected<nlohmann::json, std::string> parseJsonSafe(const HttpClient::Response &resp) {
        if (resp.status_code == 429) {
            HttpClient::RateLimiter::instance().backoff(std::chrono::seconds(2));
            return std::unexpected("Rate limited");
        }

        if (resp.status_code != 200 || resp.text.empty()) {
            return std::unexpected(std::format("HTTP error: {}", resp.status_code));
        }

        auto result = HttpClient::decode(resp);
        if (result.is_null()) {
            return std::unexpected("Failed to parse JSON");
        }

        return result;
    }

    void FetchAvatarImage(uint64_t userId) {
        g_avatarState.started = true;
        g_avatarState.loading = true;

        ThreadTask::fireAndForget([userId] {
            const std::string metaUrl = std::format(
                "https://thumbnails.roblox.com/v1/users/avatar?userIds={}&size=420x420&format=Png",
                userId
            );

            auto metaResp = HttpClient::rateLimitedGet(metaUrl);
            auto metaJsonResult = parseJsonSafe(metaResp);

            if (!metaJsonResult) {
                ThreadTask::RunOnMain([] {
                    g_avatarState.loading = false;
                    g_avatarState.failed = true;
                });
                return;
            }

            const auto &metaJson = *metaJsonResult;
            std::string avatarUrl;

            if (metaJson.contains("data") && !metaJson["data"].empty() && metaJson["data"][0].contains("imageUrl")) {
                avatarUrl = metaJson["data"][0]["imageUrl"].get<std::string>();
            }

            if (avatarUrl.empty()) {
                ThreadTask::RunOnMain([] {
                    g_avatarState.loading = false;
                    g_avatarState.failed = true;
                });
                return;
            }

            auto imgResp = HttpClient::get(avatarUrl);
            if (imgResp.status_code != 200 || imgResp.text.empty()) {
                ThreadTask::RunOnMain([] {
                    g_avatarState.loading = false;
                    g_avatarState.failed = true;
                });
                return;
            }

            std::string data = std::move(imgResp.text);
            ThreadTask::RunOnMain([data = std::move(data)]() mutable {
                auto result = LoadTextureFromMemory(data.data(), data.size());
                if (result) {
                    g_avatarState.texture = std::move(result->texture);
                    g_avatarState.imageWidth = result->width;
                    g_avatarState.imageHeight = result->height;
                    g_avatarState.failed = false;
                } else {
                    g_avatarState.failed = true;
                }
                g_avatarState.loading = false;
            });
        });
    }

    void FetchCategories(uint64_t userId, std::string cookie) {
        g_categoryState.loading = true;

        ThreadTask::fireAndForget([userId, cookie = std::move(cookie)] {
            const std::string url = std::format("https://inventory.roblox.com/v1/users/{}/categories", userId);

            auto resp = HttpClient::rateLimitedGet(
                url,
                {
                    {"Cookie", std::format(".ROBLOSECURITY={}", cookie)}
                }
            );
            auto jsonResult = parseJsonSafe(resp);

            if (!jsonResult) {
                ThreadTask::RunOnMain([] {
                    g_categoryState.loading = false;
                    g_categoryState.failed = true;
                });
                return;
            }

            const auto &j = *jsonResult;
            std::vector<CategoryInfo> categories;

            if (j.contains("categories")) {
                for (const auto &cat: j["categories"]) {
                    CategoryInfo ci;
                    ci.displayName = cat.value("displayName", "");

                    if (cat.contains("items")) {
                        for (const auto &it: cat["items"]) {
                            const int id = it.value("id", 0);
                            const std::string name = it.value("displayName", "");
                            if (id != 0) {
                                ci.assetTypes.emplace_back(id, name);
                            }
                        }
                    }

                    if (!ci.assetTypes.empty()) {
                        categories.push_back(std::move(ci));
                    }
                }
            }

            ThreadTask::RunOnMain([categories = std::move(categories)]() mutable {
                g_categoryState.categories = std::move(categories);
                g_categoryState.loading = false;
                g_categoryState.failed = g_categoryState.categories.empty();
            });
        });
    }

    void FetchEquippedItems(uint64_t userId) {
        g_equippedState.loading = true;
        g_equippedState.failed = false;

        ThreadTask::fireAndForget([userId] {
            const std::string url = std::format("https://avatar.roblox.com/v1/users/{}/currently-wearing", userId);

            auto resp = HttpClient::rateLimitedGet(url);
            auto jsonResult = parseJsonSafe(resp);

            if (!jsonResult) {
                ThreadTask::RunOnMain([userId] {
                    g_equippedState.userId = userId;
                    g_equippedState.failed = true;
                    g_equippedState.loading = false;
                });
                return;
            }

            const auto &j = *jsonResult;
            std::vector<uint64_t> ids;

            if (j.contains("assetIds")) {
                for (const auto &v: j["assetIds"]) {
                    if (v.is_number_unsigned()) {
                        uint64_t id = v.get<uint64_t>();
                        if (id != 0) {
                            ids.push_back(id);
                        }
                    }
                }
            }

            ThreadTask::RunOnMain([userId, ids = std::move(ids)]() mutable {
                if (userId != g_categoryState.userId) {
                    return;
                }

                g_equippedState.userId = userId;
                g_equippedState.assetIds = std::move(ids);
                g_equippedState.failed = g_equippedState.assetIds.empty();
                g_equippedState.loading = false;
            });
        });
    }

    void FetchThumbnailsBatch(std::vector<uint64_t> assetIds) {
        if (assetIds.empty()) {
            return;
        }

        g_batchThumbState.batchLoading = true;

        for (uint64_t id : assetIds) {
            g_thumbCache[id].loading = true;
        }
        g_activeThumbLoads += static_cast<int>(assetIds.size());

        ThreadTask::fireAndForget([assetIds = std::move(assetIds)] {
            std::string assetIdList;
            for (size_t i = 0; i < assetIds.size(); ++i) {
                if (i > 0) assetIdList += ",";
                assetIdList += std::to_string(assetIds[i]);
            }

            const std::string metaUrl = std::format(
                "https://thumbnails.roblox.com/v1/assets?assetIds={}&size=75x75&format=Png",
                assetIdList
            );

            auto metaResp = HttpClient::rateLimitedGet(metaUrl);
            auto metaJsonResult = parseJsonSafe(metaResp);

            if (!metaJsonResult) {
                ThreadTask::RunOnMain([assetIds] {
                    for (uint64_t id : assetIds) {
                        auto it = g_thumbCache.find(id);
                        if (it != g_thumbCache.end()) {
                            it->second.loading = false;
                            it->second.failed = true;
                        }
                    }
                    g_activeThumbLoads -= static_cast<int>(assetIds.size());
                    g_batchThumbState.batchLoading = false;
                });
                return;
            }

            const auto &metaJson = *metaJsonResult;

            std::unordered_map<uint64_t, std::string> imageUrls;
            if (metaJson.contains("data")) {
                for (const auto &item : metaJson["data"]) {
                    uint64_t targetId = item.value("targetId", 0ULL);
                    std::string imageUrl = item.value("imageUrl", "");
                    if (targetId != 0 && !imageUrl.empty()) {
                        imageUrls[targetId] = imageUrl;
                    }
                }
            }

            for (uint64_t assetId : assetIds) {
                auto urlIt = imageUrls.find(assetId);
                if (urlIt == imageUrls.end() || urlIt->second.empty()) {
                    ThreadTask::RunOnMain([assetId] {
                        auto it = g_thumbCache.find(assetId);
                        if (it != g_thumbCache.end()) {
                            it->second.loading = false;
                            it->second.failed = true;
                        }
                        --g_activeThumbLoads;
                    });
                    continue;
                }

                auto imgResp = HttpClient::get(urlIt->second);
                if (imgResp.status_code != 200 || imgResp.text.empty()) {
                    ThreadTask::RunOnMain([assetId] {
                        auto it = g_thumbCache.find(assetId);
                        if (it != g_thumbCache.end()) {
                            it->second.loading = false;
                            it->second.failed = true;
                        }
                        --g_activeThumbLoads;
                    });
                    continue;
                }

                std::string data = std::move(imgResp.text);
                ThreadTask::RunOnMain([assetId, data = std::move(data)]() mutable {
                    auto it = g_thumbCache.find(assetId);
                    if (it == g_thumbCache.end()) {
                        --g_activeThumbLoads;
                        return;
                    }

                    auto result = LoadTextureFromMemory(data.data(), data.size());
                    if (result) {
                        it->second.texture = std::move(result->texture);
                        it->second.width = result->width;
                        it->second.height = result->height;
                        it->second.failed = false;
                    } else {
                        it->second.failed = true;
                    }
                    it->second.loading = false;
                    --g_activeThumbLoads;
                });
            }

            ThreadTask::RunOnMain([] {
                g_batchThumbState.batchLoading = false;
            });
        });
    }

    void QueueThumbnailForBatch(uint64_t assetId) {
        auto &thumb = g_thumbCache[assetId];
        if (thumb.hasTexture() || thumb.loading || thumb.failed) {
            return;
        }

        g_batchThumbState.pendingAssetIds.push_back(assetId);

        if (!g_batchThumbState.batchLoading &&
            (g_batchThumbState.pendingAssetIds.size() >= BATCH_THUMBNAIL_SIZE ||
             g_activeThumbLoads < MAX_CONCURRENT_THUMB_LOADS)) {

            std::vector<uint64_t> batch;
            size_t count = std::min(g_batchThumbState.pendingAssetIds.size(),
                                    static_cast<size_t>(BATCH_THUMBNAIL_SIZE));

            batch.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                batch.push_back(g_batchThumbState.pendingAssetIds[i]);
            }
            g_batchThumbState.pendingAssetIds.erase(
                g_batchThumbState.pendingAssetIds.begin(),
                g_batchThumbState.pendingAssetIds.begin() + count
            );

            FetchThumbnailsBatch(std::move(batch));
        }
    }

    void FetchThumbnail(uint64_t assetId) {
        QueueThumbnailForBatch(assetId);
    }

    void FetchInventory(uint64_t userId, std::string cookie, int assetTypeId) {
        g_inventoryState.loading = true;
        g_inventoryState.failed = false;

        ThreadTask::fireAndForget([userId, cookie = std::move(cookie), assetTypeId] {
            std::vector<InventoryItem> items;
            std::string cursor;
            bool anyError = false;

            while (!anyError) {
                std::string url = std::format(
                    "https://inventory.roblox.com/v2/users/{}/inventory/{}?limit=100&sortOrder=Asc",
                    userId,
                    assetTypeId
                );

                if (!cursor.empty()) {
                    url.append(std::format("&cursor={}", cursor));
                }

                auto resp = HttpClient::rateLimitedGet(
                    url,
                    {
                        {"Cookie", std::format(".ROBLOSECURITY={}", cookie)}
                    }
                );
                auto jsonResult = parseJsonSafe(resp);

                if (!jsonResult) {
                    anyError = true;
                    break;
                }

                const auto &j = *jsonResult;

                if (j.contains("data")) {
                    for (const auto &it: j["data"]) {
                        InventoryItem ii;
                        ii.assetId = it.value("assetId", uint64_t {0});
                        ii.assetName = it.value("assetName", "");
                        items.push_back(std::move(ii));
                    }
                }

                cursor.clear();
                if (j.contains("nextPageCursor") && !j["nextPageCursor"].is_null()) {
                    cursor = j["nextPageCursor"].get<std::string>();
                }

                if (cursor.empty()) {
                    break;
                }
            }

            ThreadTask::RunOnMain([assetTypeId, anyError, items = std::move(items)]() mutable {
                if (!anyError) {
                    g_inventoryState.cachedInventories[assetTypeId] = std::move(items);
                    g_inventoryState.failed = false;
                } else {
                    g_inventoryState.failed = true;
                }
                g_inventoryState.loading = false;
            });
        });
    }

    void RenderAvatarPane(float width, uint64_t userId) {
        ImGui::BeginChild("AvatarImagePane", ImVec2(width, 0), true);

        if (g_avatarState.hasTexture() && !g_avatarState.loading) {
            const float desiredWidth = width - ImGui::GetStyle().ItemSpacing.x * 2;
            const float desiredHeight
                = (g_avatarState.imageWidth > 0)
                      ? (desiredWidth * static_cast<float>(g_avatarState.imageHeight) / g_avatarState.imageWidth)
                      : 0.0f;
            ImGui::Image(ImTextureID(g_avatarState.texture.get()), ImVec2(desiredWidth, desiredHeight));
        } else if (g_avatarState.loading) {
            ImGui::TextUnformatted("Loading avatar...");
        } else if (g_avatarState.failed) {
            ImGui::TextUnformatted("Failed to load avatar image.");
        }

        if (userId != 0 && userId != g_equippedState.userId && !g_equippedState.loading) {
            FetchEquippedItems(userId);
        }

        if (g_equippedState.loading) {
            ImGui::TextUnformatted("Fetching equipped items...");
        } else if (g_equippedState.failed) {
            ImGui::TextUnformatted("Failed to fetch equipped items.");
        } else if (!g_equippedState.assetIds.empty()) {
            const float equipMinCell = ImGui::GetFontSize() * EQUIPPED_MIN_CELL_MULTIPLIER;
            const float equipAvailX = width - ImGui::GetStyle().ItemSpacing.x * 2;
            int equipColumns = static_cast<int>(std::floor(equipAvailX / equipMinCell));
            equipColumns = std::max(equipColumns, 1);

            const float equipCellSize
                = std::floor((equipAvailX - (equipColumns - 1) * ImGui::GetStyle().ItemSpacing.x) / equipColumns);

            int index = 0;
            for (uint64_t assetId: g_equippedState.assetIds) {
                if (index % equipColumns != 0) {
                    ImGui::SameLine();
                }

                auto &thumb = g_thumbCache[assetId];
                if (!thumb.hasTexture() && !thumb.loading && !thumb.failed
                    && g_activeThumbLoads < MAX_CONCURRENT_THUMB_LOADS) {
                    QueueThumbnailForBatch(assetId);
                }

                ImGui::PushID(index);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, THUMB_ROUNDING);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

                if (thumb.hasTexture()) {
                    ImGui::ImageButton(
                        "##eq",
                        ImTextureID(thumb.texture.get()),
                        ImVec2(equipCellSize, equipCellSize),
                        ImVec2(0, 0),
                        ImVec2(1, 1),
                        ImVec4(0, 0, 0, 0),
                        ImVec4(1, 1, 1, 1)
                    );
                } else {
                    ImGui::Button("", ImVec2(equipCellSize, equipCellSize));
                }

                ImGui::PopStyleVar(2);
                ImGui::PopID();
                ++index;
            }
        }

        ImGui::EndChild();
    }

    [[nodiscard]]
    float CalculateComboWidth(const char *text) {
        const ImGuiStyle &style = ImGui::GetStyle();
        return ImGui::CalcTextSize(text).x + style.FramePadding.x * 2.0f + ImGui::GetFrameHeight();
    }

    void RenderSearchAndFilters(
        int assetTypeId,
        const std::vector<const char *> &categoryNames,
        const std::vector<const char *> &assetTypeNames
    ) {
        const ImGuiStyle &style = ImGui::GetStyle();

        const float catComboWidth = CalculateComboWidth(categoryNames[g_categoryState.selectedCategory]);
        float assetComboWidth = 0.0f;
        if (assetTypeNames.size() > 1) {
            assetComboWidth = CalculateComboWidth(assetTypeNames[g_inventoryState.selectedAssetTypeIndex]);
        }

        const float minFieldWidth = ImGui::GetFontSize() * MIN_FIELD_MULTIPLIER;
        float inputWidth = ImGui::GetContentRegionAvail().x - catComboWidth - assetComboWidth;
        if (assetComboWidth > 0) {
            inputWidth -= style.ItemSpacing.x;
        }
        inputWidth -= style.ItemSpacing.x;
        inputWidth = std::max(inputWidth, minFieldWidth);

        int itemCount = 0;
        if (auto it = g_inventoryState.cachedInventories.find(assetTypeId);
            it != g_inventoryState.cachedInventories.end()) {
            itemCount = static_cast<int>(it->second.size());
        }

        const std::string searchHint = itemCount > 0 ? std::format("Search {} items", itemCount) : "Search items";

        ImGui::PushItemWidth(inputWidth);
        ImGui::InputTextWithHint("##inventory_search", searchHint.c_str(), g_searchBuffer, SEARCH_BUFFER_SIZE);
        ImGui::PopItemWidth();

        ImGui::SameLine(0, style.ItemSpacing.x);
        ImGui::PushItemWidth(catComboWidth);
        if (ImGui::Combo(
                "##categoryCombo",
                &g_categoryState.selectedCategory,
                categoryNames.data(),
                static_cast<int>(categoryNames.size())
            )) {
            g_inventoryState.selectedAssetTypeIndex = 0;
            g_searchBuffer[0] = '\0';
        }
        ImGui::PopItemWidth();

        if (assetComboWidth > 0) {
            ImGui::SameLine(0, style.ItemSpacing.x);
            ImGui::PushItemWidth(assetComboWidth);
            ImGui::Combo(
                "##assetTypeCombo",
                &g_inventoryState.selectedAssetTypeIndex,
                assetTypeNames.data(),
                static_cast<int>(assetTypeNames.size())
            );
            ImGui::PopItemWidth();
        }

        ImGui::Separator();
    }

    void RenderInventoryGrid(const std::vector<InventoryItem> &items, float cellSize, int columns) {
        std::string filterLower;
        {
            std::string sb = g_searchBuffer;
            std::ranges::transform(sb, sb.begin(), ::tolower);
            filterLower = std::move(sb);
        }

        const std::unordered_set<uint64_t> equippedSet(
            g_equippedState.assetIds.begin(),
            g_equippedState.assetIds.end()
        );

        std::vector<int> visibleIndices;
        visibleIndices.reserve(items.size());

        int selectedIndex = -1;

        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            if (!filterLower.empty()) {
                std::string nameLower = items[i].assetName;
                std::ranges::transform(nameLower, nameLower.begin(), ::tolower);
                if (nameLower.find(filterLower) == std::string::npos) {
                    continue;
                }
            }

            if (items[i].assetId == g_selectedAssetId) {
                selectedIndex = i;
            } else {
                visibleIndices.push_back(i);
            }
        }

        if (selectedIndex != -1) {
            visibleIndices.insert(visibleIndices.begin(), selectedIndex);
        }

        const int itemCount = static_cast<int>(visibleIndices.size());
        const int rowCount = (itemCount + columns - 1) / columns;

        ImGuiListClipper clipper;
        clipper.Begin(rowCount, cellSize + ImGui::GetStyle().ItemSpacing.y);

        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const int firstIdx = row * columns;

                for (int col = 0; col < columns; ++col) {
                    const int listIdx = firstIdx + col;
                    if (listIdx >= itemCount) {
                        break;
                    }

                    const int itemIndex = visibleIndices[listIdx];
                    const auto &item = items[itemIndex];

                    if (col > 0) {
                        ImGui::SameLine();
                    }

                    auto &thumb = g_thumbCache[item.assetId];
                    if (!thumb.hasTexture() && !thumb.loading && !thumb.failed
                        && g_activeThumbLoads < MAX_CONCURRENT_THUMB_LOADS) {
                        QueueThumbnailForBatch(item.assetId);
                    }

                    ImGui::PushID(itemIndex);

                    const bool isEquipped = equippedSet.contains(item.assetId);
                    const bool isSelected = (item.assetId == g_selectedAssetId);

                    if (thumb.hasTexture()) {
                        const ImVec4 tint = isSelected ? ImVec4(1, 1, 1, 1) : ImVec4(1, 1, 1, 1);
                        const ImVec4 btnCol
                            = isEquipped ? ImGui::GetStyleColorVec4(ImGuiCol_Button) : ImVec4(0, 0, 0, 0);
                        const ImVec4 btnColHovered
                            = isEquipped ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered) : ImVec4(0, 0, 0, 0);
                        const ImVec4 btnColActive
                            = isEquipped ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImVec4(0, 0, 0, 0);

                        ImGui::PushStyleColor(ImGuiCol_Button, btnCol);
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, btnColHovered);
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, btnColActive);
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, THUMB_ROUNDING);
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

                        ImGui::ImageButton(
                            "##img",
                            ImTextureID(thumb.texture.get()),
                            ImVec2(cellSize, cellSize),
                            ImVec2(0, 0),
                            ImVec2(1, 1),
                            ImVec4(0, 0, 0, 0),
                            tint
                        );

                        ImGui::PopStyleVar(2);
                        ImGui::PopStyleColor(3);

                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", item.assetName.c_str());
                        }
                    } else {
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, THUMB_ROUNDING);
                        ImVec4 btnCol = isEquipped ? ImGui::GetStyleColorVec4(ImGuiCol_Button) : ImVec4(0, 0, 0, 0);
                        if (!isSelected) {
                            btnCol.w *= 0.7f;
                        }
                        ImGui::PushStyleColor(ImGuiCol_Button, btnCol);
                        ImGui::Button(item.assetName.c_str(), ImVec2(cellSize, cellSize));
                        ImGui::PopStyleColor();
                        ImGui::PopStyleVar();
                    }

                    if (ImGui::BeginPopupContextItem("ctx")) {
                        ImGui::MenuItem("Equip", nullptr, false, false);
                        ImGui::MenuItem("Inspect", nullptr, false, false);
                        ImGui::EndPopup();
                    }

                    const ImVec2 rectMin = ImGui::GetItemRectMin();
                    const ImVec2 rectMax = ImGui::GetItemRectMax();
                    const ImU32 outlineColor
                        = isSelected ? ImGui::GetColorU32(ImGuiCol_ButtonActive) : ImGui::GetColorU32(ImGuiCol_Border);
                    ImGui::GetWindowDrawList()->AddRect(rectMin, rectMax, outlineColor, THUMB_ROUNDING, 0, 1.0f);

                    ImGui::PopID();
                }
            }
        }
    }

    void RenderInventoryPane(uint64_t userId, std::string cookie) {
        ImGui::BeginChild("AvatarInventoryPane", ImVec2(0, 0), true);

        if (g_categoryState.loading) {
            ImGui::TextUnformatted("Loading categories...");
            ImGui::EndChild();
            return;
        }

        if (g_categoryState.failed) {
            ImGui::TextUnformatted("Failed to load categories.");
            ImGui::EndChild();
            return;
        }

        if (g_categoryState.categories.empty()) {
            ImGui::TextUnformatted("No categories available.");
            ImGui::EndChild();
            return;
        }

        std::vector<const char *> categoryNames;
        for (const auto &ci: g_categoryState.categories) {
            categoryNames.push_back(ci.displayName.c_str());
        }

        if (g_categoryState.selectedCategory >= static_cast<int>(categoryNames.size())) {
            g_categoryState.selectedCategory = 0;
        }

        std::vector<const char *> assetTypeNames;
        for (const auto &p: g_categoryState.categories[g_categoryState.selectedCategory].assetTypes) {
            assetTypeNames.push_back(p.second.c_str());
        }

        if (g_inventoryState.selectedAssetTypeIndex >= static_cast<int>(assetTypeNames.size())) {
            g_inventoryState.selectedAssetTypeIndex = 0;
        }

        const int assetTypeId = g_categoryState.categories[g_categoryState.selectedCategory]
                                    .assetTypes[g_inventoryState.selectedAssetTypeIndex]
                                    .first;

        RenderSearchAndFilters(assetTypeId, categoryNames, assetTypeNames);

        auto itInv = g_inventoryState.cachedInventories.find(assetTypeId);
        if (itInv == g_inventoryState.cachedInventories.end() && !g_inventoryState.loading) {
            FetchInventory(userId, std::move(cookie), assetTypeId);
        }

        if (g_inventoryState.loading) {
            ImGui::TextUnformatted("Loading items...");
        } else if (g_inventoryState.failed) {
            ImGui::TextUnformatted("Failed to load items.");
        } else if (itInv != g_inventoryState.cachedInventories.end()) {
            const auto &invItems = itInv->second;
            const float minCellSize = ImGui::GetFontSize() * MIN_CELL_SIZE_MULTIPLIER;
            const float availX = ImGui::GetContentRegionAvail().x;

            int columns = static_cast<int>(std::floor(availX / minCellSize));
            columns = std::max(columns, 1);

            const float cellSize = std::floor((availX - (columns - 1) * ImGui::GetStyle().ItemSpacing.x) / columns);

            RenderInventoryGrid(invItems, cellSize, columns);
        }

        ImGui::EndChild();
    }
} // namespace

void RenderInventoryTab() {
    auto [currentUserId, currentCookie] = GetCurrentUserInfo();

    if (currentUserId != g_avatarState.loadedUserId) {
        g_avatarState.started = false;
        g_avatarState.failed = false;
        g_avatarState.loading = false;
        ResetAvatarTexture();
        g_avatarState.loadedUserId = currentUserId;
    }

    if (currentUserId != g_categoryState.userId) {
        ResetAllState();
        g_categoryState.userId = currentUserId;
    }

    if (currentUserId == 0) {
        ImGui::TextUnformatted("No account selected.");
        return;
    }

    if (!g_avatarState.started) {
        FetchAvatarImage(currentUserId);
    }

    if (g_categoryState.userId != 0 && !g_categoryState.loading && g_categoryState.categories.empty()
        && !g_categoryState.failed) {
        FetchCategories(currentUserId, currentCookie);
    }

    const float availWidth = ImGui::GetContentRegionAvail().x;
    const float leftWidth = availWidth * 0.35f;

    RenderAvatarPane(leftWidth, currentUserId);
    ImGui::SameLine();
    RenderInventoryPane(currentUserId, currentCookie);
}
