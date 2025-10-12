#include "inventory.h"

#include "../data.h"
#include "system/main_thread.h"
#include "system/threading.h"
#include "ui/image.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <imgui.h>
#include <imgui_internal.h> // for ImGuiListClipper
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace ImGui;

// Structs to keep inventory data
struct InventoryItem {
		uint64_t assetId {};
		std::string assetName;
};

struct CategoryInfo {
		std::string displayName; // e.g. "Accessories"
		std::vector<std::pair<int, std::string>> assetTypes; // pair<assetTypeId, displayName>
};

struct ThumbInfo {
		ID3D11ShaderResourceView *srv {nullptr};
		int width {0};
		int height {0};
		bool loading {false};
		bool failed {false};
};

static std::unordered_map<uint64_t, ThumbInfo> s_thumbCache; // key = assetId

// Rounded corner radius for thumbnail buttons
constexpr float kThumbRounding = 6.0f;

// Selected inventory asset (for outline highlight)
static uint64_t s_selectedAssetId = 0;

// Equipped items state
static uint64_t s_equippedUserId = 0;
static bool s_equippedLoading = false;
static bool s_equippedFailed = false;
static std::vector<uint64_t> s_equippedAssetIds;

// Limit concurrent thumbnail fetches to avoid hammering the thumbnail API.
static int s_activeThumbLoads = 0;
constexpr int kMaxConcurrentThumbLoads = 24; // tweak as needed

void RenderInventoryTab() {
	// Persistent state across frames
	static ID3D11ShaderResourceView *s_texture = nullptr;
	static int s_imageWidth = 0;
	static int s_imageHeight = 0;
	static bool s_loading = false;
	static bool s_failed = false;
	static bool s_started = false;
	static uint64_t s_loadedUserId = 0; // UserId that the current texture belongs to

	static uint64_t s_catUserId = 0; // userId categories were fetched for
	static bool s_catLoading = false;
	static bool s_catFailed = false;
	static std::vector<CategoryInfo> s_categories;
	static int s_selectedCategory = 0; // tab index

	static std::unordered_map<int, std::vector<InventoryItem>> s_cachedInventories; // key = assetTypeId
	static int s_selectedAssetTypeIndex = 0; // dropdown index inside selected category
	static bool s_invLoading = false;
	static bool s_invFailed = false;

	static char s_searchBuffer[64] = "";

	// Determine which user we should show
	uint64_t currentUserId = 0;
	std::string currentCookie;
	if (!g_selectedAccountIds.empty()) {
		int internalId = *g_selectedAccountIds.begin();
		for (const auto &acc : g_accounts) {
			if (acc.id == internalId && !acc.userId.empty()) {
				try {
					currentUserId = std::stoull(acc.userId);
				} catch (...) { currentUserId = 0; }
				currentCookie = acc.cookie;
				break;
			}
		}
	} else if (g_defaultAccountId != -1) {
		for (const auto &acc : g_accounts) {
			if (acc.id == g_defaultAccountId && !acc.userId.empty()) {
				try {
					currentUserId = std::stoull(acc.userId);
				} catch (...) { currentUserId = 0; }
				currentCookie = acc.cookie;
				break;
			}
		}
	}

	// If account changed, reset state so we fetch a new avatar
	if (currentUserId != s_loadedUserId) {
		s_started = false;
		s_failed = false;
		s_loading = false;
		if (s_texture) {
			s_texture->Release();
			s_texture = nullptr;
		}
		s_loadedUserId = currentUserId;
	}

	// If the displayed user changed, clear caches
	if (currentUserId != s_catUserId) {
		s_catUserId = currentUserId;
		s_categories.clear();
		s_catLoading = false;
		s_catFailed = false;
		s_selectedCategory = 0;
		s_selectedAssetTypeIndex = 0;
		s_cachedInventories.clear();
		s_invLoading = false;
		s_invFailed = false;
		s_searchBuffer[0] = '\0';
		// Clear thumbnail cache for previous user
		for (auto &p : s_thumbCache) {
			if (p.second.srv) { p.second.srv->Release(); }
		}
		s_thumbCache.clear();
		// reset equipped list state
		s_equippedUserId = 0;
		s_equippedLoading = false;
		s_equippedFailed = false;
		s_equippedAssetIds.clear();
	}

	// Early-out if we don't have a user to show
	if (currentUserId == 0) {
		TextUnformatted("No account selected.");
		return;
	}

	// Kick off download once per userId
	if (!s_started) {
		s_started = true;
		s_loading = true;

		Threading::newThread([currentUserId] {
			// 420×420 PNG full-body avatar image
			std::string metaUrl = "https://thumbnails.roblox.com/v1/users/avatar?userIds="
								+ std::to_string(currentUserId) + "&size=420x420&format=Png";

			auto metaResp = HttpClient::get(metaUrl);
			if (metaResp.status_code != 200 || metaResp.text.empty()) {
				MainThread::Post([] {
					s_loading = false;
					s_failed = true;
				});
				return;
			}

			nlohmann::json metaJson = HttpClient::decode(metaResp);
			std::string avatarUrl;
			try {
				if (metaJson.contains("data") && !metaJson["data"].empty()
					&& metaJson["data"][0].contains("imageUrl")) {
					avatarUrl = metaJson["data"][0]["imageUrl"].get<std::string>();
				}
			} catch (...) { avatarUrl.clear(); }

			if (avatarUrl.empty()) {
				MainThread::Post([] {
					s_loading = false;
					s_failed = true;
				});
				return;
			}

			auto imgResp = HttpClient::get(avatarUrl);
			if (imgResp.status_code != 200 || imgResp.text.empty()) {
				MainThread::Post([] {
					s_loading = false;
					s_failed = true;
				});
				return;
			}

			std::string data = std::move(imgResp.text);

			MainThread::Post([data = std::move(data)]() mutable {
				if (LoadTextureFromMemory(data.data(), data.size(), &s_texture, &s_imageWidth, &s_imageHeight)) {
					s_failed = false;
				} else {
					s_failed = true;
				}
				s_loading = false;
			});
		});
	}

	// Kick off categories fetch once
	if (!s_catLoading && s_categories.empty() && !s_catFailed) {
		s_catLoading = true;
		Threading::newThread([currentUserId, cookie = currentCookie] {
			std::string url = "https://inventory.roblox.com/v1/users/" + std::to_string(currentUserId) + "/categories";
			auto resp = HttpClient::get(
				url,
				{
					{"Cookie", ".ROBLOSECURITY=" + cookie}
			}
			);
			if (resp.status_code != 200 || resp.text.empty()) {
				MainThread::Post([] {
					s_catLoading = false;
					s_catFailed = true;
				});
				return;
			}
			nlohmann::json j;
			try {
				j = HttpClient::decode(resp);
			} catch (...) {
				MainThread::Post([] {
					s_catLoading = false;
					s_catFailed = true;
				});
				return;
			}
			std::vector<CategoryInfo> categories;
			try {
				if (j.contains("categories")) {
					for (auto &cat : j["categories"]) {
						CategoryInfo ci;
						ci.displayName = cat.value("displayName", "");
						if (cat.contains("items")) {
							for (auto &it : cat["items"]) {
								int id = it.value("id", 0);
								std::string name = it.value("displayName", "");
								if (id != 0) { ci.assetTypes.emplace_back(id, name); }
							}
						}
						if (!ci.assetTypes.empty()) { categories.push_back(std::move(ci)); }
					}
				}
			} catch (...) {}
			MainThread::Post([categories = std::move(categories)]() mutable {
				s_categories = std::move(categories);
				s_catLoading = false;
				s_catFailed = s_categories.empty();
			});
		});
	}

	// Layout: left 35% width child for the avatar image
	float availWidth = GetContentRegionAvail().x;
	float leftWidth = availWidth * 0.35f;

	BeginChild("AvatarImagePane", ImVec2(leftWidth, 0), true);

	if (s_texture && !s_loading) {
		float desiredWidth = leftWidth - GetStyle().ItemSpacing.x * 2;
		float desiredHeight
			= (s_imageWidth > 0) ? (desiredWidth * static_cast<float>(s_imageHeight) / s_imageWidth) : 0.0f;
		Image(ImTextureID(reinterpret_cast<void *>(s_texture)), ImVec2(desiredWidth, desiredHeight));
	} else if (s_loading) {
		TextUnformatted("Loading avatar...");
	} else if (s_failed) {
		TextUnformatted("Failed to load avatar image.");
	}

	// --- Equipped items fetch ---
	if (currentUserId != 0 && currentUserId != s_equippedUserId && !s_equippedLoading) {
		s_equippedLoading = true;
		s_equippedFailed = false;
		Threading::newThread([uid = currentUserId]() {
			std::string url = "https://avatar.roblox.com/v1/users/" + std::to_string(uid) + "/currently-wearing";
			auto resp = HttpClient::get(url);
			if (resp.status_code != 200 || resp.text.empty()) {
				MainThread::Post([uid]() {
					s_equippedUserId = uid;
					s_equippedFailed = true;
					s_equippedLoading = false;
				});
				return;
			}
			nlohmann::json j;
			try {
				j = HttpClient::decode(resp);
			} catch (...) {
				MainThread::Post([uid]() {
					s_equippedUserId = uid;
					s_equippedFailed = true;
					s_equippedLoading = false;
				});
				return;
			}
			std::vector<uint64_t> ids;
			try {
				if (j.contains("assetIds")) {
					for (auto &v : j["assetIds"]) {
						uint64_t id = 0;
						try {
							id = v.get<uint64_t>();
						} catch (...) {}
						if (id != 0) { ids.push_back(id); }
					}
				}
			} catch (...) {}

			MainThread::Post([uid, ids = std::move(ids)]() mutable {
				// Discard if user changed while the request was in-flight
				if (uid != s_catUserId) { return; }

				s_equippedUserId = uid;
				s_equippedAssetIds = std::move(ids);
				s_equippedFailed = s_equippedAssetIds.empty();
				s_equippedLoading = false;
			});
		});
	}

	// --- Equipped items UI below avatar ---
	if (s_equippedLoading) {
		TextUnformatted("Fetching equipped items...");
	} else if (s_equippedFailed) {
		TextUnformatted("Failed to fetch equipped items.");
	} else if (!s_equippedAssetIds.empty()) {
		// Dynamic equipped items grid sizing
		float equipMinCell = GetFontSize() * 3.75f; // ~60px at 16px
		float equipAvailX = leftWidth - GetStyle().ItemSpacing.x * 2;
		int equipColumns = static_cast<int>(std::floor(equipAvailX / equipMinCell));
		if (equipColumns < 1) { equipColumns = 1; }
		float equipCellSize = (equipAvailX - (equipColumns - 1) * GetStyle().ItemSpacing.x) / equipColumns;
		equipCellSize = std::floor(equipCellSize);

		int index = 0;
		for (uint64_t aid : s_equippedAssetIds) {
			if (index % equipColumns != 0) { SameLine(); }

			auto &thumb = s_thumbCache[aid];
			if (!thumb.srv && !thumb.loading && !thumb.failed && s_activeThumbLoads < kMaxConcurrentThumbLoads) {
				thumb.loading = true;
				++s_activeThumbLoads;
				Threading::newThread([assetId = aid]() {
					// identical download logic as before
					auto finish = [assetId](bool success) {
						MainThread::Post([assetId, success]() {
							auto &ti = s_thumbCache[assetId];
							ti.loading = false;
							ti.failed = !success;
							--s_activeThumbLoads;
						});
					};
					std::string metaUrl = "https://thumbnails.roblox.com/v1/assets?assetIds=" + std::to_string(assetId)
										+ "&size=75x75&format=Png";
					auto metaResp = HttpClient::get(metaUrl);
					if (metaResp.status_code != 200 || metaResp.text.empty()) {
						finish(false);
						return;
					}
					nlohmann::json metaJson = HttpClient::decode(metaResp);
					std::string imageUrl;
					try {
						if (metaJson.contains("data") && !metaJson["data"].empty()) {
							const auto &d = metaJson["data"][0];
							if (d.contains("imageUrl")) { imageUrl = d["imageUrl"].get<std::string>(); }
						}
					} catch (...) { imageUrl.clear(); }
					if (imageUrl.empty()) {
						finish(false);
						return;
					}
					auto imgResp = HttpClient::get(imageUrl);
					if (imgResp.status_code != 200 || imgResp.text.empty()) {
						finish(false);
						return;
					}
					std::string data = std::move(imgResp.text);
					MainThread::Post([assetId, data = std::move(data)]() mutable {
						auto &ti = s_thumbCache[assetId];
						bool ok = LoadTextureFromMemory(data.data(), data.size(), &ti.srv, &ti.width, &ti.height);
						ti.loading = false;
						ti.failed = !ok;
						--s_activeThumbLoads;
					});
				});
			}

			// Ensure unique ImGui IDs for each equipped item to avoid conflicts.
			PushID(index);

			// Render equipped thumbnail
			PushStyleVar(ImGuiStyleVar_FrameRounding, kThumbRounding);
			PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
			ImVec4 tintEquip(1, 1, 1, 1);
			if (thumb.srv) {
				ImageButton(
					"##eq",
					ImTextureID(reinterpret_cast<void *>(thumb.srv)),
					ImVec2(equipCellSize, equipCellSize),
					ImVec2(0, 0),
					ImVec2(1, 1),
					ImVec4(0, 0, 0, 0),
					tintEquip
				);
			} else {
				Button("", ImVec2(equipCellSize, equipCellSize));
			}
			PopStyleVar(2);

			PopID();

			++index;
		}
	}

	EndChild();

	SameLine();

	// Right pane for inventory
	BeginChild("AvatarInventoryPane", ImVec2(0, 0), true);

	if (s_catLoading) {
		TextUnformatted("Loading categories...");
		EndChild();
		return;
	}
	if (s_catFailed) {
		TextUnformatted("Failed to load categories.");
		EndChild();
		return;
	}

	// ------------------------------
	// Search bar + Category dropdown + (optional) AssetType dropdown row
	ImGuiStyle &style = GetStyle();

	// Build category list for combo
	std::vector<const char *> categoryNames;
	for (auto &ci : s_categories) { categoryNames.push_back(ci.displayName.c_str()); }

	if (s_selectedCategory >= static_cast<int>(categoryNames.size())) { s_selectedCategory = 0; }

	// Build asset-type list for the currently selected category
	std::vector<const char *> assetTypeNames;
	for (auto &p : s_categories[s_selectedCategory].assetTypes) { assetTypeNames.push_back(p.second.c_str()); }

	if (s_selectedAssetTypeIndex >= static_cast<int>(assetTypeNames.size())) { s_selectedAssetTypeIndex = 0; }

	// Compute widths so everything fits nicely in one line
	auto calcComboWidth = [&](const char *text) {
		// Text width + padding left/right + arrow region (~frame height)
		return CalcTextSize(text).x + style.FramePadding.x * 2.0f + GetFrameHeight();
	};
	float catComboWidth = calcComboWidth(categoryNames[s_selectedCategory]);
	float assetComboWidth = 0.0f;
	if (assetTypeNames.size() > 1) { assetComboWidth = calcComboWidth(assetTypeNames[s_selectedAssetTypeIndex]); }

	float inputWidth = GetContentRegionAvail().x - catComboWidth - assetComboWidth;
	if (assetComboWidth > 0) { inputWidth -= style.ItemSpacing.x; }
	inputWidth -= style.ItemSpacing.x; // space between search and category combo
	float minField = GetFontSize() * 6.25f; // ~100px at 16px
	if (inputWidth < minField) { inputWidth = minField; }

	// Determine current asset type to display (needed for dynamic search hint)
	int assetTypeId = s_categories[s_selectedCategory].assetTypes[s_selectedAssetTypeIndex].first;

	// Build dynamic hint text like "Search 53 items" when inventory is available
	int itemCountHint = 0;
	auto itHintInv = s_cachedInventories.find(assetTypeId);
	if (itHintInv != s_cachedInventories.end()) { itemCountHint = static_cast<int>(itHintInv->second.size()); }

	std::string searchHint
		= itemCountHint > 0 ? ("Search " + std::to_string(itemCountHint) + " items") : "Search items";

	// Search input
	PushItemWidth(inputWidth);
	InputTextWithHint("##inventory_search", searchHint.c_str(), s_searchBuffer, sizeof(s_searchBuffer));
	PopItemWidth();

	// Category combo
	SameLine(0, style.ItemSpacing.x);
	PushItemWidth(catComboWidth);
	if (Combo("##categoryCombo", &s_selectedCategory, categoryNames.data(), categoryNames.size())) {
		// when user picks a new category reset sub-state
		s_selectedAssetTypeIndex = 0;
		s_searchBuffer[0] = '\0';
	}
	PopItemWidth();

	// Asset-type combo (only if category has multiple)
	if (assetComboWidth > 0) {
		SameLine(0, style.ItemSpacing.x);
		PushItemWidth(assetComboWidth);
		Combo("##assetTypeCombo", &s_selectedAssetTypeIndex, assetTypeNames.data(), assetTypeNames.size());
		PopItemWidth();
	}

	Separator();

	// Fetch inventory if not cached
	auto itInv = s_cachedInventories.find(assetTypeId);
	if (itInv == s_cachedInventories.end() && !s_invLoading) {
		s_invLoading = true;
		s_invFailed = false;
		Threading::newThread([currentUserId, cookie = currentCookie, assetTypeId] {
			std::vector<InventoryItem> items;

			std::string cursor; // pagination cursor, empty for first page
			bool anyError = false;
			while (!anyError) {
				std::string url = "https://inventory.roblox.com/v2/users/" + std::to_string(currentUserId)
								+ "/inventory/" + std::to_string(assetTypeId) + "?limit=100&sortOrder=Asc";
				if (!cursor.empty()) { url += "&cursor=" + cursor; }

				auto resp = HttpClient::get(
					url,
					{
						{"Cookie", ".ROBLOSECURITY=" + cookie}
				}
				);
				if (resp.status_code != 200 || resp.text.empty()) {
					anyError = true;
					break;
				}

				nlohmann::json j;
				try {
					j = HttpClient::decode(resp);
				} catch (...) {
					anyError = true;
					break;
				}

				try {
					if (j.contains("data")) {
						for (auto &it : j["data"]) {
							InventoryItem ii;
							ii.assetId = it.value("assetId", 0);
							ii.assetName = it.value("assetName", "");
							items.push_back(std::move(ii));
						}
					}
				} catch (...) {}

				// Check for pagination cursor
				cursor.clear();
				try {
					if (j.contains("nextPageCursor") && !j["nextPageCursor"].is_null()) {
						cursor = j["nextPageCursor"].get<std::string>();
					}
				} catch (...) {}

				if (cursor.empty()) {
					break; // no more pages
				}
			}

			MainThread::Post([assetTypeId, anyError, items = std::move(items)]() mutable {
				if (!anyError) {
					s_cachedInventories[assetTypeId] = std::move(items);
					s_invFailed = false;
				} else {
					s_invFailed = true;
				}
				s_invLoading = false;
			});
		});
	}

	// Draw inventory list / grid
	if (s_invLoading) {
		TextUnformatted("Loading items...");
	} else if (s_invFailed) {
		TextUnformatted("Failed to load items.");
	} else {
		const auto &invItems = s_cachedInventories[assetTypeId];
		std::string filterLower;
		{
			std::string sb = s_searchBuffer;
			std::transform(sb.begin(), sb.end(), sb.begin(), ::tolower);
			filterLower = std::move(sb);
		}

		float MIN_CELL_SIZE = GetFontSize() * 6.25f; // ~100px at 16px
		float availX = GetContentRegionAvail().x;
		int columns = static_cast<int>(std::floor(availX / MIN_CELL_SIZE));
		if (columns < 1) { columns = 1; }
		float cellSize = (availX - (columns - 1) * style.ItemSpacing.x) / columns;
		cellSize = std::floor(cellSize);

		// Build a fast lookup for equipped items
		std::unordered_set<uint64_t> equippedSet(s_equippedAssetIds.begin(), s_equippedAssetIds.end());

		// Build a list of indices that pass the search filter so we can clip accurately, keeping the selected item on
		// top
		std::vector<int> visibleIndices;
		visibleIndices.reserve(invItems.size());
		int selectedIndex = -1;
		for (int i = 0; i < static_cast<int>(invItems.size()); ++i) {
			if (!filterLower.empty()) {
				std::string nameLower = invItems[i].assetName;
				std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
				if (nameLower.find(filterLower) == std::string::npos) { continue; }
			}
			if (invItems[i].assetId == s_selectedAssetId) {
				selectedIndex = i;
			} else {
				visibleIndices.push_back(i);
			}
		}
		// Place selected index at the front if it matches filter
		if (selectedIndex != -1) { visibleIndices.insert(visibleIndices.begin(), selectedIndex); }

		int itemCount = static_cast<int>(visibleIndices.size());
		int rowCount = (itemCount + columns - 1) / columns;

		ImGuiListClipper clipper;
		clipper.Begin(rowCount, cellSize + style.ItemSpacing.y);
		while (clipper.Step()) {
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
				int firstIdx = row * columns;
				for (int col = 0; col < columns; ++col) {
					int listIdx = firstIdx + col;
					if (listIdx >= itemCount) { break; }

					int itemIndex = visibleIndices[listIdx];
					const auto &itm = invItems[itemIndex];

					if (col > 0) { SameLine(); }

					// Thumbnail handling (only start downloads for on-screen items)
					auto &thumb = s_thumbCache[itm.assetId];
					if (!thumb.srv && !thumb.loading && !thumb.failed
						&& s_activeThumbLoads < kMaxConcurrentThumbLoads) {
						thumb.loading = true;
						++s_activeThumbLoads;
						Threading::newThread([assetId = itm.assetId]() {
							auto finishWithState = [assetId](bool success) {
								MainThread::Post([assetId, success]() {
									auto &ti = s_thumbCache[assetId];
									ti.loading = false;
									ti.failed = !success;
									--s_activeThumbLoads;
								});
							};

							std::string metaUrl = "https://thumbnails.roblox.com/v1/assets?assetIds="
												+ std::to_string(assetId) + "&size=75x75&format=Png";
							auto metaResp = HttpClient::get(metaUrl);
							if (metaResp.status_code != 200 || metaResp.text.empty()) {
								finishWithState(false);
								return;
							}

							nlohmann::json metaJson = HttpClient::decode(metaResp);
							std::string imageUrl;
							try {
								if (metaJson.contains("data") && !metaJson["data"].empty()) {
									const auto &d = metaJson["data"][0];
									if (d.contains("imageUrl")) { imageUrl = d["imageUrl"].get<std::string>(); }
								}
							} catch (...) { imageUrl.clear(); }

							if (imageUrl.empty()) {
								finishWithState(false);
								return;
							}

							auto imgResp = HttpClient::get(imageUrl);
							if (imgResp.status_code != 200 || imgResp.text.empty()) {
								finishWithState(false);
								return;
							}

							std::string data = std::move(imgResp.text);
							MainThread::Post([assetId, data = std::move(data)]() mutable {
								auto &ti = s_thumbCache[assetId];
								bool ok
									= LoadTextureFromMemory(data.data(), data.size(), &ti.srv, &ti.width, &ti.height);
								ti.loading = false;
								ti.failed = !ok;
								--s_activeThumbLoads;
							});
						});
					}

					PushID(itemIndex);
					bool itemClicked = false;
					if (thumb.srv) {
						bool isEquipped = equippedSet.count(itm.assetId) > 0;

						// Build tint/background colours. Non-selected, non-equipped items should have a fully
						// transparent background so the accessory images blend seamlessly with the UI.
						ImVec4 tint
							= (itm.assetId == s_selectedAssetId) ? ImVec4(1, 1, 1, 1) : ImVec4(1.0f, 1.0f, 1.0f, 1);

						// Background colour only shown for equipped items – otherwise keep alpha at 0.
						ImVec4 btnCol = isEquipped ? GetStyleColorVec4(ImGuiCol_Button) : ImVec4(0, 0, 0, 0);
						ImVec4 btnColHovered
							= isEquipped ? GetStyleColorVec4(ImGuiCol_ButtonHovered) : ImVec4(0, 0, 0, 0);
						ImVec4 btnColActive
							= isEquipped ? GetStyleColorVec4(ImGuiCol_ButtonActive) : ImVec4(0, 0, 0, 0);

						// Override ImGui button colours for the duration of this draw so that non-selected,
						// non-equipped thumbnails have no visible frame/background.
						PushStyleColor(ImGuiCol_Button, btnCol);
						PushStyleColor(ImGuiCol_ButtonHovered, btnColHovered);
						PushStyleColor(ImGuiCol_ButtonActive, btnColActive);

						PushStyleVar(ImGuiStyleVar_FrameRounding, kThumbRounding);
						PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

						itemClicked = ImageButton(
							"##img",
							ImTextureID(reinterpret_cast<void *>(thumb.srv)),
							ImVec2(cellSize, cellSize),
							ImVec2(0, 0),
							ImVec2(1, 1),
							ImVec4(0, 0, 0, 0),
							tint
						);

						PopStyleVar(2);
						PopStyleColor(3);

						if (IsItemHovered()) { SetTooltip("%s", itm.assetName.c_str()); }
					} else {
						bool isEquipped = equippedSet.count(itm.assetId) > 0;
						PushStyleVar(ImGuiStyleVar_FrameRounding, kThumbRounding);
						ImVec4 btnCol = isEquipped ? GetStyleColorVec4(ImGuiCol_Button) : ImVec4(0, 0, 0, 0);
						if (itm.assetId != s_selectedAssetId) { btnCol.w *= 0.7f; }
						PushStyleColor(ImGuiCol_Button, btnCol);
						itemClicked = Button(itm.assetName.c_str(), ImVec2(cellSize, cellSize));
						PopStyleColor();
						PopStyleVar();
					}

					// Clicking no longer changes selection – disabled.

					// Context menu
					if (BeginPopupContextItem("ctx")) {
						MenuItem("Equip", nullptr, false, false); // dummy disabled
						MenuItem("Inspect", nullptr, false, false);
						EndPopup();
					}

					// Draw outline around the last item rect
					ImVec2 rectMin = GetItemRectMin();
					ImVec2 rectMax = GetItemRectMax();
					ImU32 outlineColor = (itm.assetId == s_selectedAssetId) ? GetColorU32(ImGuiCol_ButtonActive)
																			: GetColorU32(ImGuiCol_Border);
					GetWindowDrawList()->AddRect(rectMin, rectMax, outlineColor, kThumbRounding, 0, 1.0f);

					PopID();
				}
			}
		}
	}

	EndChild();
}
