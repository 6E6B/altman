#include "servers.h"
#include "servers_utils.h"

#include <array>
#include <vector>
#include <string>
#include <string_view>
#include <imgui.h>
#include "imgui_internal.h"
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <thread>
#include <utility>
#include <cstdio>
#include <ranges>
#include <format>
#include <expected>

#include "ui/ui.h"
#include "utils/account_utils.h"
#include "ui/windows/accounts/accounts_join_ui.h"
#include "ui/windows/components.h"
#include "ui/widgets/context_menus.h"
#include "console/console.h"
#include "ui/widgets/bottom_right_status.h"
#include "network/roblox/common.h"
#include "network/roblox/auth.h"
#include "network/roblox/games.h"
#include "network/roblox/session.h"
#include "network/roblox/social.h"
#include "system/roblox_launcher.h"
#include "ui/widgets/modal_popup.h"
#include "utils/thread_task.h"

namespace {
	void renderPublicServers();
	void renderPrivateServers();

	constexpr float MIN_INPUT_WIDTH = 100.0f;
	constexpr float DEFAULT_ROW_HEIGHT = 19.0f;
	constexpr int COLUMN_COUNT = 5;

	enum class ServerSortMode {
		None = 0,
		PingAsc,
		PingDesc,
		PlayersAsc,
		PlayersDesc
	};

	enum class ServerTab {
		Public,
		Private
	};

	struct SeverTabInfo {
		const char* title;
		ServerTab tabId;
		void (*renderFunction)();
	};

	struct ServersViewState {
		int viewAccountId{-1};
	};

	static ServersViewState g_serversView;

	constexpr std::array serverTabs = {
		SeverTabInfo{"Public Servers", ServerTab::Public, renderPublicServers},
		SeverTabInfo{"Private Server", ServerTab::Private, renderPrivateServers}
	};

	struct PrivateServer {
		std::string name;
		std::string ownerName;
		std::string ownerDisplayName;
		std::string universeName;
		uint64_t vipServerId;
		uint64_t placeId;
		uint64_t universeId;
		uint64_t ownerId;
		int maxPlayers;
		bool active;
		std::string expirationDate;
		bool willRenew;
		std::optional<int> priceInRobux;

		int playing;
		double fps;
		int ping;
	};

	struct ServerState {
		ServerSortMode sortMode{ServerSortMode::None};
		int sortComboIndex{0};
		std::vector<PublicServerInfo> cachedServers;
		std::unordered_map<std::string, Roblox::ServerPage> pageCache;
		std::string currentCursor;
		std::string nextCursor;
		std::string prevCursor;
		char searchBuffer[64]{};
		char placeIdBuffer[32]{};
		uint64_t currentPlaceId{0};
	};

	ServerState g_state;
	int g_activeServersTab = static_cast<int>(ServerTab::Public);

	std::string toLowerCase(std::string_view sv) {
		std::string result;
		result.reserve(sv.size());
		std::ranges::transform(sv, std::back_inserter(result),
			[](unsigned char c) { return std::tolower(c); });
		return result;
	}

	bool matchesQuery(const PublicServerInfo& server, std::string_view queryLower) {
		const auto haystack = std::format("{} {}/{} {}ms {}",
			server.jobId,
			server.currentPlayers, server.maximumPlayers,
			static_cast<int>(server.averagePing + 0.5),
			static_cast<int>(server.averageFps + 0.5));

		return toLowerCase(haystack).find(queryLower) != std::string::npos;
	}

	std::expected<uint64_t, std::string> parsePlaceId(std::string_view input) {
		std::string cleaned;
		std::ranges::copy_if(input, std::back_inserter(cleaned),
			[](char c) { return !std::isspace(static_cast<unsigned char>(c)); });

		if (cleaned.empty() || !std::ranges::all_of(cleaned,
			[](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
			return std::unexpected("Place ID must contain only digits");
		}

		char* end = nullptr;
		errno = 0;
		const uint64_t value = std::strtoull(cleaned.c_str(), &end, 10);

		if (errno == ERANGE) {
			return std::unexpected("Place ID is too large");
		}
		if (end == cleaned.c_str() || *end != '\0') {
			return std::unexpected("Invalid Place ID format");
		}

		return value;
	}

	void fetchPageServers(uint64_t placeId, std::string_view cursor = {}) {
		if (placeId != g_state.currentPlaceId) {
			g_state.pageCache.clear();
			g_state.currentPlaceId = placeId;
		}

		const std::string cursorStr(cursor);

		if (const auto it = g_state.pageCache.find(cursorStr); it != g_state.pageCache.end()) {
			const auto& page = it->second;
			g_state.cachedServers = page.data;
			g_state.nextCursor = page.nextCursor;
			g_state.prevCursor = page.prevCursor;
			g_state.currentCursor = cursorStr;
			LOG_INFO(page.data.empty() ? "No servers found for this page" : "Fetched servers");
			return;
		}

		try {
			const auto page = Roblox::getPublicServersPage(placeId, cursorStr);
			g_state.pageCache.emplace(cursorStr, page);
			g_state.cachedServers = page.data;
			g_state.nextCursor = page.nextCursor;
			g_state.prevCursor = page.prevCursor;
			g_state.currentCursor = cursorStr;
			LOG_INFO(page.data.empty() ? "No servers found for this page" : "Fetched servers");
		} catch (const std::exception& ex) {
			LOG_INFO("Fetch error: {}", ex.what());
			g_state.cachedServers.clear();
			g_state.nextCursor.clear();
			g_state.prevCursor.clear();
		}
	}

	void sortServers(std::vector<PublicServerInfo>& servers, ServerSortMode mode) {
		switch (mode) {
		case ServerSortMode::PingAsc:
			std::ranges::sort(servers, {}, &PublicServerInfo::averagePing);
			break;
		case ServerSortMode::PingDesc:
			std::ranges::sort(servers, std::ranges::greater{}, &PublicServerInfo::averagePing);
			break;
		case ServerSortMode::PlayersAsc:
			std::ranges::sort(servers, {}, &PublicServerInfo::currentPlayers);
			break;
		case ServerSortMode::PlayersDesc:
			std::ranges::sort(servers, std::ranges::greater{}, &PublicServerInfo::currentPlayers);
			break;
		case ServerSortMode::None:
		default:
			break;
		}
	}

	std::vector<PublicServerInfo> getFilteredServers() {
		const std::string queryLower = toLowerCase(g_state.searchBuffer);
		const bool isSearching = !queryLower.empty();

		std::vector<PublicServerInfo> displayList;

		if (isSearching) {
			for (const auto& [cursor, page] : g_state.pageCache) {
				for (const auto& server : page.data) {
					if (matchesQuery(server, queryLower)) {
						displayList.push_back(server);
					}
				}
			}
		} else {
			displayList = g_state.cachedServers;
		}

		if (g_state.sortMode == ServerSortMode::None && isSearching) {
			std::ranges::sort(displayList, {}, &PublicServerInfo::jobId);
		} else {
			sortServers(displayList, g_state.sortMode);
		}

		return displayList;
	}

	struct RowMetrics {
		float height;
		float verticalPadding;
	};

	RowMetrics calculateRowMetrics() {
		float height = ImGui::GetFrameHeight();
		if (height <= 0.0f) height = ImGui::GetTextLineHeightWithSpacing();
		if (height <= 0.0f) height = DEFAULT_ROW_HEIGHT;

		const float textHeight = ImGui::GetTextLineHeight();
		const float padding = std::max(0.0f, (height - textHeight) * 0.5f);

		return {height, padding};
	}

	void renderSearchControls() {
		const auto& style = ImGui::GetStyle();

		const float fetchWidth = ImGui::CalcTextSize("Fetch Servers").x + style.FramePadding.x * 2.0f;
		const float prevWidth = ImGui::CalcTextSize("\xEF\x81\x93 Prev Page").x + style.FramePadding.x * 2.0f;
		const float nextWidth = ImGui::CalcTextSize("Next Page \xEF\x81\x94").x + style.FramePadding.x * 2.0f;
		const float totalButtons = fetchWidth + prevWidth + nextWidth + style.ItemSpacing.x * 2;

		const float inputWidth = std::max(MIN_INPUT_WIDTH,
			ImGui::GetContentRegionAvail().x - totalButtons - style.ItemSpacing.x);

		ImGui::PushItemWidth(inputWidth);
		ImGui::InputTextWithHint("##placeid_servers", "Place Id",
			g_state.placeIdBuffer, sizeof(g_state.placeIdBuffer));
		ImGui::PopItemWidth();

		ImGui::SameLine(0, style.ItemSpacing.x);
		if (ImGui::Button("Fetch Servers", ImVec2(fetchWidth, 0))) {
			if (auto result = parsePlaceId(g_state.placeIdBuffer)) {
				g_state.currentCursor.clear();
				fetchPageServers(*result);
			} else {
				LOG_INFO(result.error());
			}
		}

		ImGui::SameLine(0, style.ItemSpacing.x);
		ImGui::BeginDisabled(g_state.prevCursor.empty());
		if (ImGui::Button("\xEF\x81\x93 Prev Page", ImVec2(prevWidth, 0))) {
			fetchPageServers(g_state.currentPlaceId, g_state.prevCursor);
		}
		ImGui::EndDisabled();

		ImGui::SameLine(0, style.ItemSpacing.x);
		ImGui::BeginDisabled(g_state.nextCursor.empty());
		if (ImGui::Button("Next Page \xEF\x81\x94", ImVec2(nextWidth, 0))) {
			fetchPageServers(g_state.currentPlaceId, g_state.nextCursor);
		}
		ImGui::EndDisabled();
	}

	void renderFilterControls() {
		constexpr const char* SORT_OPTIONS[] = {
			"None", "Ping (Asc)", "Ping (Desc)", "Players (Asc)", "Players (Desc)"
		};

		const auto& style = ImGui::GetStyle();
		const float comboWidth = ImGui::CalcTextSize("Players (Desc)").x + style.FramePadding.x * 7.0f;
		const float searchWidth = std::max(MIN_INPUT_WIDTH,
			ImGui::GetContentRegionAvail().x - comboWidth - style.ItemSpacing.x);

		ImGui::PushItemWidth(searchWidth);
		ImGui::InputTextWithHint("##search_servers", "Search...",
			g_state.searchBuffer, sizeof(g_state.searchBuffer));
		ImGui::PopItemWidth();

		ImGui::SameLine(0, style.ItemSpacing.x);
		ImGui::PushItemWidth(comboWidth);
		if (ImGui::Combo("##server_filter", &g_state.sortComboIndex,
			SORT_OPTIONS, IM_ARRAYSIZE(SORT_OPTIONS))) {
			g_state.sortMode = static_cast<ServerSortMode>(g_state.sortComboIndex);
		}
		ImGui::PopItemWidth();
	}

	void renderServerRow(const PublicServerInfo& server, const RowMetrics& metrics) {
		ImGui::TableNextRow();
		ImGui::PushID(server.jobId.c_str());

		ImGui::TableNextColumn();
		const float cellStartY = ImGui::GetCursorPosY();

		const auto selectableId = std::format("##JobIDSelectable_{}", server.jobId);
		if (ImGui::Selectable(selectableId.c_str(), false,
			ImGuiSelectableFlags_SpanAllColumns |
			ImGuiSelectableFlags_AllowOverlap |
			ImGuiSelectableFlags_AllowDoubleClick,
			ImVec2(0, metrics.height)) &&
			ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {

			launchWithSelectedAccounts(LaunchParams::gameJob(g_state.currentPlaceId, server.jobId));
			}

		if (ImGui::BeginPopupContextItem("ServerRowContextMenu")) {
			StandardJoinMenuParams menu{};
			menu.placeId = g_state.currentPlaceId;
			menu.universeId = g_targetUniverseId_ServersTab;
			menu.jobId = server.jobId;

			menu.onLaunchGame = [placeId = g_state.currentPlaceId]() {
				launchWithSelectedAccounts(LaunchParams::standard(placeId));
			};

			menu.onLaunchInstance = [placeId = g_state.currentPlaceId, jobId = server.jobId]() {
				if (jobId.empty())
					return;
				launchWithSelectedAccounts(LaunchParams::gameJob(placeId, jobId));
			};

			menu.onFillGame = [placeId = g_state.currentPlaceId]() {
				FillJoinOptions(placeId, "");
			};
			menu.onFillInstance = [placeId = g_state.currentPlaceId, jobId = server.jobId]() {
				FillJoinOptions(placeId, jobId);
			};

			RenderStandardJoinMenu(menu);
			ImGui::EndPopup();
		}

		ImGui::SetCursorPosY(cellStartY + metrics.verticalPadding);
		ImGui::TextUnformatted(server.jobId.c_str());
		ImGui::SetCursorPosY(cellStartY + metrics.height);

		ImGui::TableNextColumn();
		float colStartY = ImGui::GetCursorPosY();
		ImGui::SetCursorPosY(colStartY + metrics.verticalPadding);
		const auto playersText = std::format("{}/{}", server.currentPlayers, server.maximumPlayers);
		ImGui::TextUnformatted(playersText.c_str());
		ImGui::SetCursorPosY(colStartY + metrics.height);

		ImGui::TableNextColumn();
		colStartY = ImGui::GetCursorPosY();
		ImGui::SetCursorPosY(colStartY + metrics.verticalPadding);
		const auto pingText = std::format("{:.0f} ms", server.averagePing);
		ImGui::TextUnformatted(pingText.c_str());
		ImGui::SetCursorPosY(colStartY + metrics.height);

		ImGui::TableNextColumn();
		colStartY = ImGui::GetCursorPosY();
		ImGui::SetCursorPosY(colStartY + metrics.verticalPadding);
		const auto fpsText = std::format("{:.0f}", server.averageFps);
		ImGui::TextUnformatted(fpsText.c_str());
		ImGui::SetCursorPosY(colStartY + metrics.height);

		ImGui::TableNextColumn();
		colStartY = ImGui::GetCursorPosY();
		ImGui::SetCursorPosY(colStartY + metrics.verticalPadding);
		if (ImGui::Button("Join", ImVec2(-1, 0))) {
			launchWithSelectedAccounts(LaunchParams::gameJob(g_state.currentPlaceId, server.jobId));
		}
		ImGui::SetCursorPosY(colStartY + metrics.height);

		ImGui::PopID();
	}

	void renderServerTable(const std::vector<PublicServerInfo>& servers) {
		constexpr ImGuiTableFlags tableFlags =
			ImGuiTableFlags_Borders |
			ImGuiTableFlags_RowBg |
			ImGuiTableFlags_Resizable |
			ImGuiTableFlags_ScrollY |
			ImGuiTableFlags_Hideable |
			ImGuiTableFlags_Reorderable;

		if (!ImGui::BeginTable("ServersTable", COLUMN_COUNT, tableFlags,
			ImVec2(0, ImGui::GetContentRegionAvail().y))) {
			return;
			}

		const float baseFontSize = ImGui::GetFontSize();
		ImGui::TableSetupColumn("Job ID", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, baseFontSize * 5.0f);
		ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthFixed, baseFontSize * 4.375f);
		ImGui::TableSetupColumn("FPS", ImGuiTableColumnFlags_WidthFixed, baseFontSize * 4.375f);
		ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, baseFontSize * 5.0f);
		ImGui::TableSetupScrollFreeze(0, 1);

		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		ImGui::TableNextColumn(); ImGui::TextUnformatted("Job ID");
		ImGui::TableNextColumn(); ImGui::TextUnformatted("Players");
		ImGui::TableNextColumn(); ImGui::TextUnformatted("Ping");
		ImGui::TableNextColumn(); ImGui::TextUnformatted("FPS");
		ImGui::TableNextColumn(); ImGui::TextUnformatted("Actions");

		const auto metrics = calculateRowMetrics();
		for (const auto& server : servers) {
			renderServerRow(server, metrics);
		}

		ImGui::EndTable();
	}

	void renderPublicServers() {
		renderSearchControls();
		ImGui::Separator();
		renderFilterControls();

		const auto displayList = getFilteredServers();
		renderServerTable(displayList);
	}

	class PrivateServerUI {
		public:
			int selectedTab = 0;
	private:
		std::vector<PrivateServer> servers;
		std::unordered_map<uint64_t, std::vector<size_t>> serversByPlaceId;
		char searchFilter[256] = "";
		bool isLoading = false;
		std::string errorMessage;
		std::optional<std::string> nextPageCursor;
		std::string currentCursor;
		std::vector<std::string> cursorHistory;

	public:
		void loadServers(int tabType, const AccountData& account, const std::string& cursor = {}) {
			isLoading = true;
			errorMessage.clear();

			ThreadTask::fireAndForget([this, tabType, cookie = account.cookie, cursor]() {
				try {
					auto page = Roblox::getAllPrivateServers(tabType, cookie, cursor);

					servers.clear();
					for (const auto& info : page.data) {
						PrivateServer server;
						server.name = info.name;
						server.ownerName = info.ownerName;
						server.universeName = info.universeName;
						server.vipServerId = info.privateServerId;
						server.placeId = info.placeId;
						server.universeId = info.universeId;
						server.ownerId = info.ownerId;
						server.active = info.active;
						server.expirationDate = info.expirationDate;
						server.willRenew = info.willRenew;
						server.priceInRobux = info.priceInRobux;
						server.ownerDisplayName = info.ownerName;
						server.maxPlayers = 0;
						server.playing = 0;
						server.fps = 0.0f;
						server.ping = 0;
						servers.push_back(std::move(server));
						//serversByPlaceId[info.placeId].push_back(i);
					}

					/*for (const auto& [placeId, indices] : serversByPlaceId) {
						auto gamePage = Roblox::getPrivateServersForGame(placeId, cookie);

						std::unordered_map<uint64_t, const Roblox::GamePrivateServerInfo*> byVipId;
						for (const auto& gs : gamePage.data) {
							byVipId[gs.vipServerId] = &gs;
						}

						for (size_t idx : indices) {
							auto it = byVipId.find(servers[idx].vipServerId);
							if (it != byVipId.end()) {
								const auto* gs = it->second;
								servers[idx].playing = gs->playing;
								servers[idx].fps = gs->fps;
								servers[idx].ping = gs->ping;
								servers[idx].ownerDisplayName = gs->ownerDisplayName;
								servers[idx].maxPlayers = gs->maxPlayers;
							}
						}
					}*/

					nextPageCursor = page.nextCursor;
					currentCursor = cursor;

					LOG_INFO("Loaded {} private servers", servers.size());
				}
				catch (const std::exception& ex) {
					errorMessage = std::format("Error loading servers: {}", ex.what());
					LOG_ERROR(errorMessage);
				}

				isLoading = false;
			});
		}

		void joinServer(const PrivateServer& server, const std::string& cookie) {
			auto accountPtrs = getUsableSelectedAccounts();
			if (accountPtrs.empty()) {
				LOG_INFO("No account selected to join server");
				ButtonRightStatus::Error("No account selected to join server");
				ModalPopup::AddInfo("Select an account first.");
				return;
			}

			// Copy for thread safety
			std::vector<AccountData> accounts;
			accounts.reserve(accountPtrs.size());
			for (AccountData* acc : accountPtrs) {
				accounts.push_back(*acc);
			}

			LOG_INFO("Joining private server: {}", server.name);

			ThreadTask::fireAndForget([server, accounts = std::move(accounts), cookie]() {
				try {
					auto page = Roblox::getPrivateServersForGame(server.placeId, cookie);

					std::string accessCode;
					for (const auto& gameServer : page.data) {
						if (gameServer.vipServerId == server.vipServerId) {
							accessCode = gameServer.accessCode;
							break;
						}
					}

					if (accessCode.empty()) {
						LOG_ERROR("Failed to get access code for private server");
						ButtonRightStatus::Error("Failed to get access code");
						return;
					}

					launchWithAccounts(LaunchParams::privateServerDirect(server.placeId, accessCode), accounts);
				}
				catch (const std::exception& ex) {
					LOG_ERROR("Failed to join private server: {}", ex.what());
					ButtonRightStatus::Error("Failed to join server");
				}
			});
		}

		void render(const AccountData& account) {
			const auto& style = ImGui::GetStyle();

			if (!errorMessage.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
				ImGui::TextWrapped("%s", errorMessage.c_str());
				ImGui::PopStyleColor();
				ImGui::Separator();
			}

			if (ImGui::BeginTabBar("PrivateServerTabs")) {
				if (ImGui::BeginTabItem("Joinable Servers")) {
					if (selectedTab != 1) {
						selectedTab = 1;
						currentCursor.clear();
						cursorHistory.clear();
						loadServers(1, account);
					}
					ImGui::EndTabItem();
				}

				if (ImGui::BeginTabItem("My Servers")) {
					if (selectedTab != 0) {
						selectedTab = 0;
						currentCursor.clear();
						cursorHistory.clear();
						loadServers(0, account);
					}
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}

			ImGui::Spacing();

			const float refreshWidth = ImGui::CalcTextSize("Refresh").x + style.FramePadding.x * 2.0f;
			const float prevWidth = ImGui::CalcTextSize("\xEF\x81\x93 Prev").x + style.FramePadding.x * 2.0f;
			const float nextWidth = ImGui::CalcTextSize("Next \xEF\x81\x94").x + style.FramePadding.x * 2.0f;
			const float buttonsWidth = refreshWidth + prevWidth + nextWidth + style.ItemSpacing.x * 2;
			const float searchWidth = std::max(MIN_INPUT_WIDTH,
				ImGui::GetContentRegionAvail().x - buttonsWidth - style.ItemSpacing.x);

			ImGui::PushItemWidth(searchWidth);
			ImGui::InputTextWithHint("##private_search", "Search servers...",
				searchFilter, sizeof(searchFilter));
			ImGui::PopItemWidth();

			ImGui::SameLine(0, style.ItemSpacing.x);
			if (ImGui::Button("Refresh", ImVec2(refreshWidth, 0))) {
				currentCursor.clear();
				cursorHistory.clear();
				loadServers(selectedTab, account);
			}

			ImGui::SameLine(0, style.ItemSpacing.x);
			ImGui::BeginDisabled(cursorHistory.empty() || isLoading);
			if (ImGui::Button("\xEF\x81\x93 Prev", ImVec2(prevWidth, 0))) {
				if (!cursorHistory.empty()) {
					std::string previousCursor = cursorHistory.back();
					cursorHistory.pop_back();
					loadServers(selectedTab, account, previousCursor);
				}
			}
			ImGui::EndDisabled();

			ImGui::SameLine(0, style.ItemSpacing.x);
			ImGui::BeginDisabled(!nextPageCursor.has_value() || isLoading);
			if (ImGui::Button("Next \xEF\x81\x94", ImVec2(nextWidth, 0))) {
				if (nextPageCursor.has_value()) {
					cursorHistory.push_back(currentCursor);
					loadServers(selectedTab, account, nextPageCursor.value());
				}
			}
			ImGui::EndDisabled();

			ImGui::Separator();

			if (isLoading) {
				ImGui::Text("Loading servers...");
				return;
			}

			if (servers.empty() && errorMessage.empty()) {
				ImGui::Text("No servers found");
				return;
			}

			const std::string filterLower = toLowerCase(searchFilter);
			std::vector<PrivateServer> displayList;

			for (const auto& server : servers) {
				if (!filterLower.empty()) {
					const std::string name = toLowerCase(server.name);
					const std::string game = toLowerCase(server.universeName);
					const std::string owner = toLowerCase(server.ownerDisplayName);

					if (name.find(filterLower) == std::string::npos &&
						game.find(filterLower) == std::string::npos &&
						owner.find(filterLower) == std::string::npos) {
						continue;
						}
				}
				displayList.push_back(server);
			}

			renderTable(displayList, account.cookie);
		}

	private:
		void renderTable(const std::vector<PrivateServer>& displayList, const std::string& cookie) {
			RowMetrics metrics = calculateRowMetrics();
			const int columnCount = selectedTab == 1 ? 4 : 5;
			constexpr ImGuiTableFlags flags =
				ImGuiTableFlags_Borders |
				ImGuiTableFlags_RowBg |
				ImGuiTableFlags_ScrollY |
				ImGuiTableFlags_Resizable;

			if (!ImGui::BeginTable("PrivateServersTable", columnCount, flags,
				ImVec2(0, ImGui::GetContentRegionAvail().y - 30))) {
				return;
			}

			const float base = ImGui::GetFontSize();

			ImGui::TableSetupColumn("Server Name", ImGuiTableColumnFlags_WidthFixed, base * 11.25f);
			ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch);

			if (selectedTab == 0) {
				ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, base * 4.375f);
				ImGui::TableSetupColumn("Renew", ImGuiTableColumnFlags_WidthFixed, base * 3.75f);
			} else {
				ImGui::TableSetupColumn("Owner", ImGuiTableColumnFlags_WidthFixed, base * 8.125f);
			}

			ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, base * 5.0f);
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();

			for (const auto& server : displayList) {
				ImGui::TableNextRow();
				ImGui::PushID(static_cast<int>(server.vipServerId));

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(server.name.c_str());

				ImGui::TableNextColumn();
				ImGui::TextWrapped("%s", server.universeName.c_str());

				if (selectedTab == 0) {
					ImGui::TableNextColumn();
					if (server.active) {
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
						ImGui::TextUnformatted("Active");
						ImGui::PopStyleColor();
					} else {
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
						ImGui::TextUnformatted("Inactive");
						ImGui::PopStyleColor();
					}

					ImGui::TableNextColumn();
					ImGui::TextUnformatted(server.willRenew ? "Yes" : "No");
				} else {
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(server.ownerDisplayName.c_str());
					if (ImGui::IsItemHovered() && !server.ownerName.empty()) {
						ImGui::BeginTooltip();
						ImGui::Text("Username: %s", server.ownerName.c_str());
						ImGui::EndTooltip();
					}
				}

				ImGui::TableNextColumn();
				if (ImGui::Button("Join", ImVec2(-1, 0))) {
					joinServer(server, cookie);
				}

				if (ImGui::IsItemHovered()) {
					ImGui::BeginTooltip();
					ImGui::Text("Server ID: %llu", server.vipServerId);
					ImGui::Text("Place ID: %llu", server.placeId);
					if (selectedTab == 0) {
						if (!server.expirationDate.empty()) {
							ImGui::Text("Expires: %s", server.expirationDate.c_str());
						}
						if (server.priceInRobux.has_value()) {
							ImGui::Text("Price: %d Robux", server.priceInRobux.value());
						}
					}
					ImGui::EndTooltip();
				}

				ImGui::PopID();
			}

			ImGui::EndTable();

			ImGui::Separator();
			ImGui::Text("Total servers: %zu", displayList.size());
			if (nextPageCursor.has_value()) {
				ImGui::SameLine();
				ImGui::TextUnformatted("| More results available");
			}
		}
	};

	void renderPrivateServers() {
		if (g_selectedAccountIds.empty()) {
			ImGui::TextDisabled("Select an account in the Accounts tab to view private servers.");
			return;
		}

		const int primaryId = *g_selectedAccountIds.begin();
		const AccountData* account = getAccountById(primaryId);

		if (!account) {
			ImGui::TextDisabled("Selected account not found.");
			return;
		}

		if (!AccountFilters::IsAccountUsable(*account)) {
			ImGui::TextDisabled("Selected account is not usable.");
			return;
		}

		static PrivateServerUI ui;
		static int lastAccountId = -1;

		if (lastAccountId != primaryId) {
			lastAccountId = primaryId;
			ui.loadServers(ui.selectedTab == 1 ? 1 : 0, *account);
		}

		ui.render(*account);
	}
}

void ServerTab_SearchPlace(uint64_t placeId) {
	std::snprintf(g_state.placeIdBuffer, sizeof(g_state.placeIdBuffer),
				  "%llu", static_cast<unsigned long long>(placeId));
	fetchPageServers(placeId);
}

void RenderServersTab() {
	if (g_targetPlaceId_ServersTab != 0) {
		std::snprintf(g_state.placeIdBuffer, sizeof(g_state.placeIdBuffer),
					 "%llu", static_cast<unsigned long long>(g_targetPlaceId_ServersTab));
		fetchPageServers(g_targetPlaceId_ServersTab);
		g_targetPlaceId_ServersTab = 0;
	}

	auto& style = ImGui::GetStyle();
	style.FrameRounding = 2.5f;
	style.ChildRounding = 2.5f;

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
		ImVec2(style.FramePadding.x + 2.0f, style.FramePadding.y + 2.0f));

	if (ImGui::BeginTabBar("ServersTitlebar", ImGuiTabBarFlags_Reorderable)) {
		for (const auto& tab : serverTabs) {
			const ImGuiTabItemFlags flags = (g_activeServersTab == static_cast<int>(tab.tabId))
				? ImGuiTabItemFlags_SetSelected
				: ImGuiTabItemFlags_None;

			bool opened = ImGui::BeginTabItem(tab.title, nullptr, flags);

			if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
				g_activeServersTab = static_cast<int>(tab.tabId);

			if (opened) {
				tab.renderFunction();
				ImGui::EndTabItem();
			}
		}
		ImGui::EndTabBar();
	}

	ImGui::PopStyleVar();
}