#include "games.h"

#include <format>
#include <nlohmann/json.hpp>
#include <cpr/cpr.h>

#include "ui/windows/components.h"
#include "network/http.h"
#include "common.h"
#include "console/console.h"

namespace Roblox {

GameDetail getGameDetail(uint64_t universeId) {
    using nlohmann::json;
    const std::string url =
        "https://games.roblox.com/v1/games?universeIds=" + std::to_string(universeId);

    HttpClient::Response resp = HttpClient::get(url);
    if (resp.status_code < 200 || resp.status_code >= 300) {
        LOG_ERROR("Game detail fetch failed: HTTP {}", resp.status_code);
        return GameDetail{};
    }

    GameDetail d;
    try {
        json root = json::parse(resp.text);
        if (root.contains("data") && root["data"].is_array() && !root["data"].empty()) {
            const auto& j = root["data"][0];
            d.name = j.value("name", "");
            d.genre = j.value("genre", "");
            d.genreL1 = j.value("genre_l1", "");
            d.genreL2 = j.value("genre_l2", "");
            d.description = j.value("description", "");
            d.visits = j.value("visits", 0ULL);
            d.favorites = j.value("favoritedCount", 0ULL);
            d.playing = j.value("playing", 0);
            d.maxPlayers = j.value("maxPlayers", 0);

            if (j.contains("price") && !j["price"].is_null()) {
                d.priceRobux = j["price"].get<int>();
            } else {
                d.priceRobux = -1;
            }

            d.createdIso = j.value("created", "");
            d.updatedIso = j.value("updated", "");

            if (j.contains("creator")) {
                const auto& c = j["creator"];
                d.creatorName = c.value("name", "");
                d.creatorId = c.value("id", 0ULL);
                d.creatorType = c.value("type", "");
                d.creatorVerified = c.value("hasVerifiedBadge", false);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse game detail: {}", e.what());
    }

    return d;
}

ServerPage getPublicServersPage(uint64_t placeId, const std::string& cursor) {
    std::string url =
        "https://games.roblox.com/v1/games/" + std::to_string(placeId) +
        "/servers/Public?sortOrder=Asc&limit=100" +
        (cursor.empty() ? "" : "&cursor=" + cursor);

    HttpClient::Response resp = HttpClient::get(url);
    if (resp.status_code < 200 || resp.status_code >= 300) {
        LOG_ERROR("Failed to fetch servers: HTTP {}", resp.status_code);
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
        for (auto& e : json["data"]) {
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

std::vector<GameInfo> searchGames(const std::string& query) {
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
        LOG_ERROR("Game search failed: HTTP {}", resp.status_code);
        return out;
    }

    auto j = HttpClient::decode(resp);

    if (j.contains("searchResults") && j["searchResults"].is_array()) {
        for (auto& group : j["searchResults"]) {
            if (group.value("contentGroupType", "") != "Game")
                continue;

            if (!group.contains("contents") || !group["contents"].is_array())
                continue;

            for (auto& g : group["contents"]) {
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

GamePrivateServersPage getPrivateServersForGame(uint64_t placeId, const std::string& cookie) {
    std::string url = std::format(
        "https://games.roblox.com/v1/games/{}/private-servers"
        "?excludeFriendServers=false&limit=25",
        placeId
    );

    auto resp = HttpClient::get(
        url,
        {
            {"Cookie", ".ROBLOSECURITY=" + cookie},
            {"User-Agent", "Mozilla/5.0"}
        }
    );

    if (resp.status_code < 200 || resp.status_code >= 300) {
        LOG_ERROR("Failed to fetch private servers: HTTP {}", resp.status_code);
        return {};
    }

    auto json = HttpClient::decode(resp);
    GamePrivateServersPage page;

    page.gameJoinRestricted = json.value("gameJoinRestricted", false);

    if (json.contains("nextPageCursor") && !json["nextPageCursor"].is_null()) {
        page.nextCursor = json["nextPageCursor"].get<std::string>();
    }

    if (json.contains("previousPageCursor") &&
        !json["previousPageCursor"].is_null()) {
        page.prevCursor = json["previousPageCursor"].get<std::string>();
    }

    if (!json.contains("data") || !json["data"].is_array()) {
        return page;
    }

    for (const auto& e : json["data"]) {
        GamePrivateServerInfo s;

        s.serverId = e.value("id", "");
        s.name = e.value("name", "");
        s.vipServerId = e.value("vipServerId", 0ULL);
        s.maxPlayers = e.value("maxPlayers", 0);
        s.accessCode = e.value("accessCode", "");

        s.playing = e.value("playing", 0);
        s.fps = e.value("fps", 0.0);
        s.ping = e.value("ping", 0);

        if (e.contains("players") && e["players"].is_array()) {
            for (const auto& p : e["players"]) {
                GamePrivateServerPlayer pl;
                pl.id = p.value("id", 0ULL);
                pl.name = p.value("name", "");
                pl.displayName = p.value("displayName", "");
                s.players.push_back(std::move(pl));
            }
        }

        if (e.contains("owner")) {
            const auto& o = e["owner"];
            s.ownerName = o.value("name", "");
            s.ownerDisplayName = o.value("displayName", "");
            s.ownerId = o.value("id", 0ULL);
            s.ownerVerified = o.value("hasVerifiedBadge", false);
        }

        page.data.push_back(std::move(s));
    }

    return page;
}

MyPrivateServersPage getAllPrivateServers(int serverTab, const std::string& cookie, const std::string& cursor) {
    std::string url = std::format(
        "https://games.roblox.com/v1/private-servers/my-private-servers"
        "?privateServersTab={}&itemsPerPage=100{}",
        serverTab,
        cursor.empty() ? "" : "&cursor=" + cursor
    );

    auto resp = HttpClient::get(
        url,
        {
            {"Cookie", ".ROBLOSECURITY=" + cookie},
            {"User-Agent", "Mozilla/5.0"}
        }
    );

    if (resp.status_code < 200 || resp.status_code >= 300) {
        LOG_ERROR("Failed to fetch private servers: HTTP {}", resp.status_code);
        return {};
    }

    auto json = HttpClient::decode(resp);
    MyPrivateServersPage page;

    if (json.contains("nextPageCursor") && !json["nextPageCursor"].is_null()) {
        page.nextCursor = json["nextPageCursor"].get<std::string>();
    }

    if (json.contains("previousPageCursor") &&
        !json["previousPageCursor"].is_null()) {
        page.prevCursor = json["previousPageCursor"].get<std::string>();
    }

    if (!json.contains("data") || !json["data"].is_array()) {
        return page;
    }

    for (const auto& e : json["data"]) {
        MyPrivateServerInfo s;

        s.privateServerId = e.value("privateServerId", 0ULL);
        s.universeId = e.value("universeId", 0ULL);
        s.placeId = e.value("placeId", 0ULL);
        s.ownerId = e.value("ownerId", 0ULL);
        s.ownerName = e.value("ownerName", "");
        s.name = e.value("name", "");
        s.universeName = e.value("universeName", "");
        s.expirationDate = e.value("expirationDate", "");
        s.active = e.value("active", false);
        s.willRenew = e.value("willRenew", false);

        if (e.contains("priceInRobux") && !e["priceInRobux"].is_null()) {
            s.priceInRobux = e["priceInRobux"].get<int>();
        }

        page.data.push_back(std::move(s));
    }

    return page;
}

}