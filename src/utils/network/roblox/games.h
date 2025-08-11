#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "http.hpp"
#include "core/logging.hpp"
#include "status.h"

#include "../../components/components.h"


namespace Roblox {
        struct GameDetail {
                std::string name;
                std::string genre;
                std::string genreL1;
                std::string genreL2;
                std::string description;
                uint64_t visits = 0;
                uint64_t favorites = 0;
                int playing = 0;
                int maxPlayers = 0;
                int priceRobux = -1; // -1 when price is null/unknown
                std::string createdIso;
                std::string updatedIso;

                std::string creatorName;
                uint64_t creatorId = 0;
                std::string creatorType;
                bool creatorVerified = false;
        };

	inline GameDetail getGameDetail(uint64_t universeId) {
		using nlohmann::json;
		const std::string url =
				"https://games.roblox.com/v1/games?universeIds=" + std::to_string(universeId);

                HttpClient::Response resp = HttpClient::get(url);
                if (resp.status_code < 200 || resp.status_code >= 300) {
                        LOG_ERROR("Game detail fetch failed: HTTP " + std::to_string(resp.status_code));
                        return GameDetail{};
                }

		GameDetail d;
		try {
			json root = json::parse(resp.text);
			if (root.contains("data") && root["data"].is_array() && !root["data"].empty()) {
				const auto &j = root["data"][0];
                        d.name = j.value("name", "");
                        d.genre = j.value("genre", "");
                        d.genreL1 = j.value("genre_l1", "");
                        d.genreL2 = j.value("genre_l2", "");
                        d.description = j.value("description", "");
                        d.visits = j.value("visits", 0ULL);
                        d.favorites = j.value("favoritedCount", 0ULL);
                        d.playing = j.value("playing", 0);
                        d.maxPlayers = j.value("maxPlayers", 0);
                        // price can be null; handle as -1 when not present
                        if (j.contains("price") && !j["price"].is_null()) {
                                d.priceRobux = j["price"].get<int>();
                        } else {
                                d.priceRobux = -1;
                        }
                        d.createdIso = j.value("created", "");
                        d.updatedIso = j.value("updated", "");

                        if (j.contains("creator")) {
					const auto &c = j["creator"];
                                d.creatorName = c.value("name", "");
                                d.creatorId = c.value("id", 0ULL);
                                d.creatorType = c.value("type", "");
					d.creatorVerified = c.value("hasVerifiedBadge", false);
				}
			}
		} catch (const std::exception &e) {
			LOG_ERROR(std::string("Failed to parse game detail: ") + e.what());
		}

		return d;
	}

	struct ServerPage {
		std::vector<PublicServerInfo> data;
		std::string nextCursor;
		std::string prevCursor;
	};

	static ServerPage getPublicServersPage(uint64_t placeId,
	                                       const std::string &cursor = {}) {
		std::string url =
				"https://games.roblox.com/v1/games/" + std::to_string(placeId) +
				"/servers/Public?sortOrder=Asc&limit=100" +
				(cursor.empty() ? "" : "&cursor=" + cursor);

                HttpClient::Response resp = HttpClient::get(url);
                if (resp.status_code < 200 || resp.status_code >= 300) {
                        LOG_ERROR("Failed to fetch servers: HTTP " + std::to_string(resp.status_code));
                        return ServerPage{};
                }

		auto json = HttpClient::decode(resp);

		ServerPage page;
		if (json.contains("nextPageCursor")) {
			page.nextCursor = json["nextPageCursor"].is_null()
				                  ? std::string{}
				                  : json["nextPageCursor"].get<std::string>();
		}

		if (json.contains("previousPageCursor")) {
			page.prevCursor = json["previousPageCursor"].is_null()
				                  ? std::string{}
				                  : json["previousPageCursor"].get<std::string>();
		}

		if (json.contains("data") && json["data"].is_array()) {
			for (auto &e: json["data"]) {
				PublicServerInfo s;
				s.jobId = e.value("id", "");
				s.currentPlayers = e.value("playing", 0);
				s.maximumPlayers = e.value("maxPlayers", 0);
				s.averagePing = e.value("ping", 0.0);
				s.averageFps = e.value("fps", 0.0);
				page.data.push_back(std::move(s));
			}
		}
		return page;
	}

	static std::vector<GameInfo> searchGames(const std::string &query) {
		const std::string sessionId = generateSessionId();
		auto resp = HttpClient::get(
			"https://apis.roblox.com/search-api/omni-search",
			{{"Accept", "application/json"}},
			cpr::Parameters{
				{"searchQuery", query},
				{"pageToken", ""},
				{"sessionId", sessionId},
				{"pageType", "all"}
			});

                std::vector<GameInfo> out;
                if (resp.status_code < 200 || resp.status_code >= 300) {
                        LOG_ERROR("Game search failed: HTTP " + std::to_string(resp.status_code));
                        return out;
                }

		auto j = HttpClient::decode(resp);

		if (j.contains("searchResults") && j["searchResults"].is_array()) {
			for (auto &group: j["searchResults"]) {
				if (group.value("contentGroupType", "") != "Game")
					continue;

				if (!group.contains("contents") || !group["contents"].is_array())
					continue;

				for (auto &g: group["contents"]) {
					GameInfo info;
					info.name = g.value("name", "");
					info.universeId = g.value("universeId", 0ULL);
					info.placeId = g.value("rootPlaceId", 0ULL);
					info.playerCount = g.value("playerCount", 0);
					info.upVotes = g.value("totalUpVotes", 0);
					info.downVotes = g.value("totalDownVotes", 0);
					info.creatorName = g.value("creatorName", "");
					info.creatorVerified = g.value("creatorHasVerifiedBadge", false);
					out.push_back(std::move(info));
				}
			}
		}

		return out;
	}
}
