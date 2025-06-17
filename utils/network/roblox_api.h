#pragma once

#include <iostream>
#include <random>
#include <string>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include "http.hpp"
#include "logging.hpp"
#include "status.h"
#include "threading.h"

#include "../../components/components.h"

using namespace std;

static ImVec4 getStatusColor(string statusCode) {
    if (statusCode == "Online") {
        return ImVec4(0.6f, 0.8f, 0.95f, 1.0f);
    }
    if (statusCode == "InGame") {
        return ImVec4(0.6f, 0.9f, 0.7f, 1.0f);
    }
    if (statusCode == "InStudio") {
        return ImVec4(1.0f, 0.85f, 0.7f, 1.0f);
    }
    if (statusCode == "Invisible") {
        return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    }
    if (statusCode == "Banned") {
        return ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
    }
    return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
}

static string generateSessionId() {
    static auto hex = "0123456789abcdef";
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> dis(0, 15);

    string uuid(36, ' ');
    for (int i = 0; i < 36; i++) {
        switch (i) {
            case 8:
            case 13:
            case 18:
            case 23:
                uuid[i] = '-';
                break;
            case 14:
                uuid[i] = '4';
                break;
            case 19:
                uuid[i] = hex[(dis(gen) & 0x3) | 0x8];
                break;
            default:
                uuid[i] = hex[dis(gen)];
        }
    }
    return uuid;
}

static string presenceTypeToString(int type) {
    switch (type) {
        case 1:
            return "Online";
        case 2:
            return "InGame";
        case 3:
            return "InStudio";
        case 4:
            return "Invisible";
        default:
            return "Offline";
    }
}

namespace RobloxApi {
    enum class BanCheckResult {
        InvalidCookie,
        Unbanned,
        Banned
    };
    static vector<FriendInfo> getFriends(const string &userId, const string &cookie) {
        LOG_INFO("Fetching friends list");

        HttpClient::Response resp = HttpClient::get(
            "https://friends.roblox.com/v1/users/" + userId + "/friends",
            {{"Cookie", ".ROBLOSECURITY=" + cookie}});

        if (resp.status_code != 200) {
            LOG_ERROR("Failed to fetch friends: HTTP " + to_string(resp.status_code));
            return {};
        }

        nlohmann::json j = HttpClient::decode(resp);
        vector<FriendInfo> friends;
        if (j.contains("data") && j["data"].is_array()) {
            for (const auto &item: j["data"]) {
                FriendInfo f;
                f.id = item.value("id", 0ULL);
                f.displayName = item.value("displayName", "");
                f.username = item.value("name", "");
                friends.push_back(f);
            }
        }
        return friends;
    }

    static FriendInfo getUserInfo(const string &userId) {
        LOG_INFO("Fetching user info");
        HttpClient::Response resp = HttpClient::get(
            "https://users.roblox.com/v1/users/" + userId,
            {{"Accept", "application/json"}});

        if (resp.status_code != 200) {
            LOG_ERROR("Failed to fetch user info: HTTP " + to_string(resp.status_code));
            return FriendInfo{};
        }

        nlohmann::json j = HttpClient::decode(resp);
        FriendInfo f;
        if (!j.is_null()) {
            f.id = j.value("id", 0ULL);
            f.username = j.value("name", "");
            f.displayName = j.value("displayName", "");
        }
        return f;
    }

    struct GameDetail {
        string genre;
        string description;
        uint64_t visits = 0;
        int maxPlayers = 0;
        string createdIso;
        string updatedIso;

        string creatorName;
        bool creatorVerified = false;
    };

    inline GameDetail getGameDetail(uint64_t universeId) {
        using nlohmann::json;
        const string url =
                "https://games.roblox.com/v1/games?universeIds=" + to_string(universeId);

        HttpClient::Response resp = HttpClient::get(url);
        if (resp.status_code != 200) {
            LOG_ERROR("Game detail fetch failed: HTTP " + to_string(resp.status_code));
            return GameDetail{};
        }

        GameDetail d;
        try {
            json root = json::parse(resp.text);
            if (root.contains("data") && root["data"].is_array() && !root["data"].empty()) {
                const auto &j = root["data"][0];
                d.genre = j.value("genre", "");
                d.description = j.value("description", "");
                d.visits = j.value("visits", 0ULL);
                d.maxPlayers = j.value("maxPlayers", 0);
                d.createdIso = j.value("created", "");
                d.updatedIso = j.value("updated", "");

                if (j.contains("creator")) {
                    const auto &c = j["creator"];
                    d.creatorName = c.value("name", "");
                    d.creatorVerified = c.value("hasVerifiedBadge", false);
                }
            }
        } catch (const std::exception &e) {
            LOG_ERROR(std::string("Failed to parse game detail: ") + e.what());
        }

        return d;
    }

    struct ServerPage {
        vector<PublicServerInfo> data;
        string nextCursor;
        string prevCursor;
    };

    static ServerPage getPublicServersPage(uint64_t placeId,
                                           const string &cursor = {}) {
        string url =
                "https://games.roblox.com/v1/games/" + to_string(placeId) +
                "/servers/Public?sortOrder=Asc&limit=100" +
                (cursor.empty() ? "" : "&cursor=" + cursor);

        HttpClient::Response resp = HttpClient::get(url);
        if (resp.status_code != 200) {
            LOG_ERROR("Failed to fetch servers: HTTP " + to_string(resp.status_code));
            return ServerPage{};
        }

        auto json = HttpClient::decode(resp);

        ServerPage page;
        if (json.contains("nextPageCursor")) {
            page.nextCursor = json["nextPageCursor"].is_null()
                                  ? string{}
                                  : json["nextPageCursor"].get<string>();
        }

        if (json.contains("previousPageCursor")) {
            page.prevCursor = json["previousPageCursor"].is_null()
                                  ? string{}
                                  : json["previousPageCursor"].get<string>();
        }

        if (json.contains("data") && json["data"].is_array()) {
            for (auto &e: json["data"]) {
                PublicServerInfo s;
                s.jobId = e.value("id", "");
                s.currentPlayers = e.value("playing", 0);
                s.maximumPlayers = e.value("maxPlayers", 0);
                s.averagePing = e.value("ping", 0.0);
                s.averageFps = e.value("fps", 0.0);
                page.data.push_back(move(s));
            }
        }
        return page;
    }

    static vector<GameInfo> searchGames(const string &query) {
        const string sessionId = generateSessionId();
        auto resp = HttpClient::get(
            "https://apis.roblox.com/search-api/omni-search",
            {{"Accept", "application/json"}},
            cpr::Parameters{
                {"searchQuery", query},
                {"pageToken", ""},
                {"sessionId", sessionId},
                {"pageType", "all"}
            });

        vector<GameInfo> out;
        if (resp.status_code != 200) {
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
                    out.push_back(move(info));
                }
            }
        }

        return out;
    }

    static nlohmann::json getAuthenticatedUser(const string &cookie) {
        LOG_INFO("Fetching profile info");
        HttpClient::Response response = HttpClient::get(
            "https://users.roblox.com/v1/users/authenticated",
            {{"Cookie", ".ROBLOSECURITY=" + cookie}});

        if (response.status_code != 200) {
            LOG_ERROR("Failed to fetch user info: HTTP " + to_string(response.status_code));
            return nlohmann::json::object();
        }

        return HttpClient::decode(response);
    }

    static BanCheckResult checkBanStatus(const string &cookie) {
        LOG_INFO("Checking moderation status");
        HttpClient::Response response = HttpClient::get(
            "https://usermoderation.roblox.com/v1/not-approved",
            {{"Cookie", ".ROBLOSECURITY=" + cookie}});

        if (response.status_code != 200) {
            LOG_ERROR("Failed moderation check: HTTP " + to_string(response.status_code));
            return BanCheckResult::InvalidCookie;
        }

        auto j = HttpClient::decode(response);
        if (j.is_object() && j.contains("punishmentTypeDescription"))
            return BanCheckResult::Banned;
        if (j.empty())
            return BanCheckResult::Unbanned;
        return BanCheckResult::Unbanned;
    }

    static bool isCookieValid(const string &cookie) {
        return checkBanStatus(cookie) != BanCheckResult::InvalidCookie;
    }

    static string fetchAuthTicket(const string &cookie) {
        LOG_INFO("Fetching x-csrf token");
        cout << cookie;
        auto csrfResponse = HttpClient::post(
            "https://auth.roblox.com/v1/authentication-ticket",
            {{"Cookie", ".ROBLOSECURITY=" + cookie}});

        auto csrfToken = csrfResponse.headers.find("x-csrf-token");
        if (csrfToken == csrfResponse.headers.end()) {
            cerr << "failed to get CSRF token\n";

            LOG_INFO("Failed to get CSRF token");
            return "";
        }

        LOG_INFO("Fetching authentication ticket");
        auto ticketResponse = HttpClient::post(
            "https://auth.roblox.com/v1/authentication-ticket",
            {
                {"Cookie", ".ROBLOSECURITY=" + cookie},
                {"Origin", "https://www.roblox.com"},
                {"Referer", "https://www.roblox.com/"},
                {"X-CSRF-TOKEN", csrfToken->second}
            });

        auto ticket = ticketResponse.headers.find("rbx-authentication-ticket");
        if (ticket == ticketResponse.headers.end()) {
            cerr << "failed to get authentication ticket\n";
            LOG_INFO("Failed to get authentication ticket");
            return "";
        }

        return ticket->second;
    }

    static uint64_t getUserId(const string &cookie) {
        auto userJson = getAuthenticatedUser(cookie);
        return userJson.value("id", 0ULL);
    }

    static string getUsername(const string &cookie) {
        auto userJson = getAuthenticatedUser(cookie);
        return userJson.value("name", "");
    }

    static string getDisplayName(const string &cookie) {
        auto userJson = getAuthenticatedUser(cookie);
        return userJson.value("displayName", "");
    }

    static string presenceTypeToString(int type) {
        switch (type) {
            case 1:
                return "Online";
            case 2:
                return "InGame";
            case 3:
                return "InStudio";
            case 4:
                return "Invisible";
            default:
                return "Offline";
        }
    }

    static string getPresence(
        const string &cookie,
        uint64_t userId) {
        LOG_INFO("Fetching user presence");
        nlohmann::json payload = {{"userIds", {userId}}};
        HttpClient::Response response = HttpClient::post(
            "https://presence.roproxy.com/v1/presence/users",
            {{"Cookie", ".ROBLOSECURITY=" + cookie}},
            payload.dump());
        if (response.status_code != 200) {
            LOG_ERROR("Presence lookup failed: HTTP " + to_string(response.status_code));

            // If we get HTTP 403, it likely means the user is banned or inaccessible
            if (response.status_code == 403) {
                return "Banned";
            }

            return "Offline";
        }

        // Print the raw response for debugging
        OutputDebugStringA(("Raw response: " + response.text).c_str());
        LOG_INFO("Raw response body: " + response.text);

        auto json = HttpClient::decode(response);

        // Print the parsed JSON for debugging
        OutputDebugStringA(("Parsed JSON: " + json.dump()).c_str());
        LOG_INFO("Parsed JSON: " + json.dump());

        if (json.contains("userPresences") && json["userPresences"].is_array() && !json["userPresences"].empty()) {
            const auto &jsonData = json["userPresences"][0];
            int typeInt = jsonData.value("userPresenceType", 0);
            LOG_INFO("Got user presence for " + to_string(userId));
            return presenceTypeToString(typeInt);
        }
        return "Offline";
    }

    struct VoiceSettings {
        std::string status;
        time_t bannedUntil = 0;
    };

    static VoiceSettings getVoiceChatStatus(const string &cookie) {
        LOG_INFO("Fetching voice chat settings");
        auto resp = HttpClient::get(
            "https://voice.roblox.com/v1/settings",
            {{"Cookie", ".ROBLOSECURITY=" + cookie}}
        );

        if (resp.status_code != 200) {
            LOG_INFO("Failed to fetch voice settings: HTTP " +
                to_string(resp.status_code));
            if (resp.status_code == 403)
                return {"Banned", 0};
            return {"Unknown", 0};
        }

        auto j = HttpClient::decode(resp);
        bool banned = j.value("isBanned", false);
        bool enabled = j.value("isVoiceEnabled", false);
        bool eligible = j.value("isUserEligible", false);
        bool opted = j.value("isUserOptIn", false);
        time_t bannedUntil = 0;
        if (j.contains("bannedUntil") && !j["bannedUntil"].is_null()) {
            if (j["bannedUntil"].contains("Seconds"))
                bannedUntil = j["bannedUntil"]["Seconds"].get<int64_t>();
        }

        if (banned)
            return {"Banned", bannedUntil};
        if (enabled || opted)
            return {"Enabled", 0};
        if (eligible)
            return {"Disabled", 0};

        return {"Disabled", 0};
    }

    struct FriendDetail {
        uint64_t id = 0;
        string username;
        string displayName;
        string description;
        string createdIso;
        int followers = 0;
        int following = 0;
        int placeVisits = 0;
        string presence;
    };

    static FriendDetail getUserDetails(const string &userId,
                                       const string &cookie) {
        FriendDetail d;
        mutex m;
        condition_variable cv;
        int remaining = 3;

        auto signalDone = [&] {
            lock_guard<mutex> lk(m);
            if (--remaining == 0)
                cv.notify_one();
        };

        Threading::newThread([&, userId] {
            auto resp = HttpClient::get(
                "https://users.roblox.com/v1/users/" + userId,
                {{"Accept", "application/json"}}
            );
            if (resp.status_code == 200) {
                nlohmann::json j = HttpClient::decode(resp);
                d.id = j.value("id", 0ULL);
                d.username = j.value("name", "");
                d.displayName = j.value("displayName", "");
                d.description = j.value("description", "");
                d.createdIso = j.value("created", "");
            }
            signalDone();
        });

        Threading::newThread([&, userId] {
            auto resp = HttpClient::get(
                "https://friends.roblox.com/v1/users/" + userId + "/followers/count",
                {}
            );
            if (resp.status_code == 200) {
                try {
                    d.followers = nlohmann::json::parse(resp.text).value("count", 0);
                } catch (const std::exception &e) {
                    LOG_ERROR(std::string("Failed to parse followers count: ") + e.what());
                }
            }
            signalDone();
        });

        Threading::newThread([&, userId] {
            auto resp = HttpClient::get(
                "https://friends.roblox.com/v1/users/" + userId + "/followings/count",
                {}
            );
            if (resp.status_code == 200) {
                try {
                    d.following = nlohmann::json::parse(resp.text).value("count", 0);
                } catch (const std::exception &e) {
                    LOG_ERROR(std::string("Failed to parse following count: ") + e.what());
                }
            }
            signalDone();
        });

        unique_lock<mutex> lk(m);
        cv.wait(lk, [&] {
            return remaining == 0;
        });

        return d;
    }

    struct PresenceData {
        string presence;
        string lastLocation;
        uint64_t placeId = 0;
        string gameId;
    };


    static unordered_map<uint64_t, PresenceData>
    getPresences(const vector<uint64_t> &userIds,
                 const string &cookie) {
        nlohmann::json payload = {{"userIds", userIds}};

        auto resp = HttpClient::post(
            "https://presence.roblox.com/v1/presence/users",
            {{"Cookie", ".ROBLOSECURITY=" + cookie}},
            payload.dump());

        if (resp.status_code != 200) {
            LOG_ERROR("Batch presence failed: HTTP " + to_string(resp.status_code));
            return {};
        }

        nlohmann::json j = HttpClient::decode(resp);
        unordered_map<uint64_t, PresenceData> out;

        if (j.contains("userPresences") && j["userPresences"].is_array()) {
            for (auto &up: j["userPresences"]) {
                PresenceData d;
                d.presence = presenceTypeToString(up.value("userPresenceType", 0));
                d.lastLocation = up.value("lastLocation", "");
                if (up.contains("placeId") && up["placeId"].is_number_unsigned())
                    d.placeId = up["placeId"].get<uint64_t>();
                if (up.contains("gameId") && !up["gameId"].is_null())
                    d.gameId = up["gameId"].get<string>();
                if (up.contains("userId"))
                    out[up["userId"].get<uint64_t>()] = move(d);
            }
        }
        return out;
    }

    inline uint64_t getUserIdFromUsername(const string &username) {
        nlohmann::json payload = {
            {"usernames", {username}},
            {"excludeBannedUsers", true}
        };

        auto resp = HttpClient::post(
            "https://users.roblox.com/v1/usernames/users",
            {},
            payload.dump());

        if (resp.status_code != 200) {
            LOG_ERROR("Username lookup failed: HTTP " + to_string(resp.status_code));
            return 0;
        }

        auto j = HttpClient::decode(resp);
        if (!j.contains("data") || j["data"].empty()) {
            LOG_ERROR("Username not found");
            return 0;
        }

        return j["data"][0].value("id", 0ULL);
    }

    inline bool sendFriendRequest(const string &targetUserId,
                                  const string &cookie,
                                  string *outResponse = nullptr) {
        string url = "https://friends.roblox.com/v1/users/" + targetUserId +
                     "/request-friendship";

        auto csrfResp = HttpClient::post(url, {{"Cookie", ".ROBLOSECURITY=" + cookie}});
        auto it = csrfResp.headers.find("x-csrf-token");
        if (it == csrfResp.headers.end()) {
            if (outResponse) *outResponse = "Missing CSRF token";
            cerr << "friend request: missing CSRF token\n";
            return false;
        }

        nlohmann::json body = {
            {"friendshipOriginSourceType", 0}
        };

        auto resp = HttpClient::post(
            url,
            {
                {"Cookie", ".ROBLOSECURITY=" + cookie},
                {"Origin", "https://www.roblox.com"},
                {"Referer", "https://www.roblox.com/"},
                {"X-CSRF-TOKEN", it->second}
            },
            body.dump());

        if (outResponse) *outResponse = resp.text;

        if (resp.status_code != 200) {
            cerr << "friend request failed HTTP " << resp.status_code << ": " << resp.text << "\n";
            return false;
        }

        auto j = HttpClient::decode(resp);
        bool success = j.value("success", false);
        if (success) {
            cerr << "friend request success: " << resp.text << "\n";
        } else {
            cerr << "friend request API failure: " << resp.text << "\n";
        }
        return success;
    }

    inline bool unfriend(const string &targetUserId,
                         const string &cookie,
                         string *outResponse = nullptr) {
        string url = "https://friends.roblox.com/v1/users/" + targetUserId +
                     "/unfriend";

        auto csrfResp = HttpClient::post(url, {{"Cookie", ".ROBLOSECURITY=" + cookie}});
        auto it = csrfResp.headers.find("x-csrf-token");
        if (it == csrfResp.headers.end()) {
            if (outResponse) *outResponse = "Missing CSRF token";
            return false;
        }

        auto resp = HttpClient::post(
            url,
            {
                {"Cookie", ".ROBLOSECURITY=" + cookie},
                {"Origin", "https://www.roblox.com"},
                {"Referer", "https://www.roblox.com/"},
                {"X-CSRF-TOKEN", it->second}
            }
        );

        if (outResponse) *outResponse = resp.text;

        if (resp.status_code != 200) {
            cerr << "unfriend failed HTTP " << resp.status_code << ": " << resp.text << "\n";
            return false;
        }

        return true;
    }

    inline bool followUser(const string &targetUserId, const string &cookie, string *outResponse = nullptr) {
        string url = "https://friends.roblox.com/v1/users/" + targetUserId + "/follow";

        auto csrfResp = HttpClient::post(url, {{"Cookie", ".ROBLOSECURITY=" + cookie}});
        auto it = csrfResp.headers.find("x-csrf-token");
        if (it == csrfResp.headers.end()) {
            if (outResponse) *outResponse = "Missing CSRF token";
            return false;
        }

        auto resp = HttpClient::post(
            url,
            {
                {"Cookie", ".ROBLOSECURITY=" + cookie},
                {"Origin", "https://www.roblox.com"},
                {"Referer", "https://www.roblox.com/"},
                {"X-CSRF-TOKEN", it->second}
            }
        );

        if (outResponse) *outResponse = resp.text;
        return resp.status_code == 200;
    }

    inline bool unfollowUser(const string &targetUserId, const string &cookie, string *outResponse = nullptr) {
        string url = "https://friends.roblox.com/v1/users/" + targetUserId + "/unfollow";

        auto csrfResp = HttpClient::post(url, {{"Cookie", ".ROBLOSECURITY=" + cookie}});
        auto it = csrfResp.headers.find("x-csrf-token");
        if (it == csrfResp.headers.end()) {
            if (outResponse) *outResponse = "Missing CSRF token";
            return false;
        }

        auto resp = HttpClient::post(
            url,
            {
                {"Cookie", ".ROBLOSECURITY=" + cookie},
                {"Origin", "https://www.roblox.com"},
                {"Referer", "https://www.roblox.com/"},
                {"X-CSRF-TOKEN", it->second}
            }
        );

        if (outResponse) *outResponse = resp.text;
        return resp.status_code == 200;
    }

    inline bool blockUser(const string &targetUserId, const string &cookie, string *outResponse = nullptr) {
        string url = "https://www.roblox.com/users/" + targetUserId + "/block";

        auto csrfResp = HttpClient::post(url, {{"Cookie", ".ROBLOSECURITY=" + cookie}});
        auto it = csrfResp.headers.find("x-csrf-token");
        if (it == csrfResp.headers.end()) {
            if (outResponse) *outResponse = "Missing CSRF token";
            return false;
        }

        auto resp = HttpClient::post(
            url,
            {
                {"Cookie", ".ROBLOSECURITY=" + cookie},
                {"Origin", "https://www.roblox.com"},
                {"Referer", "https://www.roblox.com/"},
                {"X-CSRF-TOKEN", it->second}
            }
        );

        if (outResponse) *outResponse = resp.text;
        return resp.status_code == 200;
    }
}
