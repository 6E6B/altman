#include "social.h"

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <format>
#include <mutex>

#include <nlohmann/json.hpp>

#include "common.h"
#include "auth.h"
#include "console/console.h"
#include "network/http.h"
#include "ui/windows/components.h"
#include "utils/thread_task.h"

namespace Roblox {

    std::vector<FriendInfo> getFriends(const std::string &userId, const std::string &cookie) {
        if (!canUseCookie(cookie)) {
            return {};
        }

        LOG_INFO("Fetching friends list");

        HttpClient::Response resp = HttpClient::get(
            "https://friends.roblox.com/v1/users/" + userId + "/friends",
            {
                {"Cookie",     ".ROBLOSECURITY=" + cookie                                        },
                {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"    },
                {"Accept",     "application/json"                                                }
            }
        );

        if (resp.status_code < 200 || resp.status_code >= 300) {
            LOG_ERROR("Failed to fetch friends: HTTP {}", resp.status_code);
            return {};
        }

        nlohmann::json friendsData = HttpClient::decode(resp);
        std::vector<FriendInfo> friends;

        if (!friendsData.contains("data") || !friendsData["data"].is_array()) {
            LOG_ERROR("Invalid response format - missing or invalid 'data' array");
            return {};
        }

        std::vector<uint64_t> friendIds;
        for (const auto &item: friendsData["data"]) {
            if (item.contains("id") && !item.value("isDeleted", false)) {
                friendIds.push_back(item["id"].get<uint64_t>());
            }
        }

        if (friendIds.empty()) {
            return friends;
        }

        constexpr size_t BATCH_SIZE = 100;
        for (size_t i = 0; i < friendIds.size(); i += BATCH_SIZE) {
            size_t end = std::min(i + BATCH_SIZE, friendIds.size());

            nlohmann::json requestBody = {
                {"fields",  {"names.combinedName", "names.username", "names.displayName"}},
                {"userIds", nlohmann::json::array()                                      }
            };

            for (size_t j = i; j < end; j++) {
                requestBody["userIds"].push_back(friendIds[j]);
            }

            HttpClient::Response profileResp = authenticatedPost(
                "https://apis.roblox.com/user-profile-api/v1/user/profiles/get-profiles",
                cookie,
                requestBody.dump()
            );

            if (profileResp.status_code < 200 || profileResp.status_code >= 300) {
                LOG_ERROR("Failed to fetch user profiles: HTTP {}", profileResp.status_code);
                continue;
            }

            nlohmann::json profileData = HttpClient::decode(profileResp);

            if (profileData.contains("profileDetails") && profileData["profileDetails"].is_array()) {
                for (const auto &profile: profileData["profileDetails"]) {
                    FriendInfo f;
                    f.id = profile.value("userId", 0ULL);

                    if (profile.contains("names")) {
                        const auto &names = profile["names"];
                        if (names.contains("displayName")) {
                            f.displayName = names["displayName"].get<std::string>();
                        }
                        if (names.contains("username")) {
                            f.username = names["username"].get<std::string>();
                        }
                        if (names.contains("combinedName")) {
                            f.displayName = names["combinedName"].get<std::string>();
                        }
                    }

                    friends.push_back(f);
                }
            }
        }

        LOG_INFO("Fetched {} friends", friends.size());

        if (friends.size() >= 1000) {
            LOG_WARN("Friend list may be at the 1000 friend limit");
        }

        return friends;
    }

    ApiResult<std::vector<FriendInfo>> getFriendsList(const std::string &userId, const std::string &cookie) {
        ApiError validationError = validateCookieForRequest(cookie);
        if (validationError != ApiError::Success) {
            return std::unexpected(validationError);
        }

        auto friends = getFriends(userId, cookie);
        return friends;
    }

    FriendInfo getUserInfo(const std::string &userId) {
        LOG_INFO("Fetching user info for {}", userId);

        HttpClient::Response resp = HttpClient::get(
            "https://users.roblox.com/v1/users/" + userId,
            {
                {"Accept", "application/json"}
            }
        );

        if (resp.status_code < 200 || resp.status_code >= 300) {
            LOG_ERROR("Failed to fetch user info: HTTP {}", resp.status_code);
            return FriendInfo {};
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

    ApiResult<FriendInfo> getUserInfoResult(const std::string &userId) {
        auto info = getUserInfo(userId);
        if (info.id == 0) {
            return std::unexpected(ApiError::NotFound);
        }
        return info;
    }

    FriendDetail getUserDetails(const std::string &userId, const std::string &cookie) {
        if (!canUseCookie(cookie)) {
            return FriendDetail {};
        }

        FriendDetail d;
        std::mutex m;
        std::condition_variable cv;
        int remaining = 4;

        auto signalDone = [&] {
            std::lock_guard<std::mutex> lk(m);
            if (--remaining == 0) {
                cv.notify_one();
            }
        };

        ThreadTask::fireAndForget([&, userId] {
            auto resp = HttpClient::get(
                "https://users.roblox.com/v1/users/" + userId,
                {
                    {"Accept", "application/json"}
                }
            );
            if (resp.status_code >= 200 && resp.status_code < 300) {
                nlohmann::json j = HttpClient::decode(resp);
                d.id = j.value("id", 0ULL);
                d.username = j.value("name", "");
                d.displayName = j.value("displayName", "");
                d.description = j.value("description", "");
                d.createdIso = j.value("created", "");
            }
            signalDone();
        });

        ThreadTask::fireAndForget([&, userId] {
            auto resp = HttpClient::get("https://friends.roblox.com/v1/users/" + userId + "/followers/count", {});
            if (resp.status_code >= 200 && resp.status_code < 300) {
                try {
                    d.followers = nlohmann::json::parse(resp.text).value("count", 0);
                } catch (const std::exception &e) {
                    LOG_ERROR("Failed to parse followers count: {}", e.what());
                }
            }
            signalDone();
        });

        ThreadTask::fireAndForget([&, userId] {
            auto resp = HttpClient::get("https://friends.roblox.com/v1/users/" + userId + "/followings/count", {});
            if (resp.status_code >= 200 && resp.status_code < 300) {
                try {
                    d.following = nlohmann::json::parse(resp.text).value("count", 0);
                } catch (const std::exception &e) {
                    LOG_ERROR("Failed to parse following count: {}", e.what());
                }
            }
            signalDone();
        });

        ThreadTask::fireAndForget([&, userId] {
            auto resp = HttpClient::get("https://friends.roblox.com/v1/users/" + userId + "/friends/count", {});
            if (resp.status_code >= 200 && resp.status_code < 300) {
                try {
                    d.friends = nlohmann::json::parse(resp.text).value("count", 0);
                } catch (const std::exception &e) {
                    LOG_ERROR("Failed to parse friends count: {}", e.what());
                }
            }
            signalDone();
        });

        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] {
            return remaining == 0;
        });

        return d;
    }

    ApiResult<FriendDetail> getUserDetailsResult(const std::string &userId, const std::string &cookie) {
        ApiError validationError = validateCookieForRequest(cookie);
        if (validationError != ApiError::Success) {
            return std::unexpected(validationError);
        }

        auto details = getUserDetails(userId, cookie);
        if (details.id == 0) {
            return std::unexpected(ApiError::NotFound);
        }
        return details;
    }

    uint64_t getUserIdFromUsername(const std::string &username) {
        nlohmann::json payload = {
            {"usernames",          {username}},
            {"excludeBannedUsers", true      }
        };

        auto resp = HttpClient::post(
            "https://users.roblox.com/v1/usernames/users",
            {
                {"Content-Type", "application/json"}
            },
            payload.dump()
        );

        if (resp.status_code < 200 || resp.status_code >= 300) {
            LOG_ERROR("Username lookup failed: HTTP {}", resp.status_code);
            return 0;
        }

        auto j = HttpClient::decode(resp);
        if (!j.contains("data") || j["data"].empty()) {
            LOG_ERROR("Username not found: {}", username);
            return 0;
        }

        return j["data"][0].value("id", 0ULL);
    }

    ApiResult<uint64_t> getUserIdFromUsernameResult(const std::string &username) {
        uint64_t id = getUserIdFromUsername(username);
        if (id == 0) {
            return std::unexpected(ApiError::NotFound);
        }
        return id;
    }

    FriendRequestsPage getIncomingFriendRequests(const std::string &cookie, const std::string &cursor, int limit) {
        FriendRequestsPage page;
        if (!canUseCookie(cookie)) {
            return page;
        }

        std::string url = "https://friends.roblox.com/v1/my/friends/requests?limit=" + std::to_string(limit);
        if (!cursor.empty()) {
            url += "&cursor=" + cursor;
        }

        HttpClient::Response resp = HttpClient::get(
            url,
            {
                {"Cookie",     ".ROBLOSECURITY=" + cookie},
                {"User-Agent", "Mozilla/5.0"             },
                {"Accept",     "application/json"        }
            }
        );

        if (resp.status_code < 200 || resp.status_code >= 300) {
            LOG_ERROR("Failed to fetch incoming friend requests: HTTP {}", resp.status_code);
            return page;
        }

        nlohmann::json j = HttpClient::decode(resp);
        if (!j.is_object()) {
            return page;
        }

        if (j.contains("nextPageCursor") && j["nextPageCursor"].is_string()) {
            page.nextCursor = j["nextPageCursor"].get_ref<const std::string &>();
        }

        if (j.contains("previousPageCursor") && j["previousPageCursor"].is_string()) {
            page.prevCursor = j["previousPageCursor"].get_ref<const std::string &>();
        }

        std::vector<uint64_t> userIds;
        std::unordered_map<uint64_t, IncomingFriendRequest> map;

        const auto dataIt = j.find("data");
        if (dataIt == j.end() || !dataIt->is_array()) {
            return page;
        }

        for (const auto &it: *dataIt) {
            if (!it.is_object()) {
                continue;
            }

            IncomingFriendRequest r;

            if (it.contains("id")) {
                if (!jsonToU64(it["id"], r.userId)) {
                    continue;
                }
                userIds.push_back(r.userId);
            } else {
                continue;
            }

            if (it.contains("friendRequest") && it["friendRequest"].is_object()) {
                const auto &fr = it["friendRequest"];

                if (fr.contains("sentAt") && fr["sentAt"].is_string()) {
                    r.sentAt = fr["sentAt"].get_ref<const std::string &>();
                }

                if (fr.contains("originSourceType")) {
                    jsonToString(fr["originSourceType"], r.originSourceType);
                }

                if (fr.contains("sourceUniverseId")) {
                    jsonToU64(fr["sourceUniverseId"], r.sourceUniverseId);
                }
            }

            if (it.contains("mutualFriendsList") && it["mutualFriendsList"].is_array()) {
                for (const auto &m: it["mutualFriendsList"]) {
                    if (m.is_string()) {
                        r.mutuals.emplace_back(m.get_ref<const std::string &>());
                    }
                }
            }

            map.emplace(r.userId, std::move(r));
        }

        if (userIds.empty()) {
            return page;
        }

        constexpr size_t BATCH = 100;
        for (size_t i = 0; i < userIds.size(); i += BATCH) {
            size_t end = std::min(i + BATCH, userIds.size());

            nlohmann::json body;
            body["userIds"] = nlohmann::json::array();
            body["fields"] = {"names.combinedName", "names.username", "isVerified", "isDeleted"};

            for (size_t k = i; k < end; ++k) {
                body["userIds"].push_back(userIds[k]);
            }

            HttpClient::Response pr = authenticatedPost(
                "https://apis.roblox.com/user-profile-api/v1/user/profiles/get-profiles",
                cookie,
                body.dump()
            );

            if (pr.status_code < 200 || pr.status_code >= 300) {
                continue;
            }

            nlohmann::json pj = HttpClient::decode(pr);
            if (!pj.is_object()) {
                continue;
            }

            const auto pit = pj.find("profileDetails");
            if (pit == pj.end() || !pit->is_array()) {
                continue;
            }

            for (const auto &p: *pit) {
                if (!p.is_object()) {
                    continue;
                }

                uint64_t uid {};
                if (!p.contains("userId") || !jsonToU64(p["userId"], uid)) {
                    continue;
                }

                auto it = map.find(uid);
                if (it == map.end()) {
                    continue;
                }

                if (p.contains("names") && p["names"].is_object()) {
                    const auto &n = p["names"];
                    if (n.contains("username") && n["username"].is_string()) {
                        it->second.username = n["username"].get_ref<const std::string &>();
                    }
                    if (n.contains("combinedName") && n["combinedName"].is_string()) {
                        it->second.displayName = n["combinedName"].get_ref<const std::string &>();
                    }
                }
            }
        }

        page.data.reserve(userIds.size());
        for (uint64_t id: userIds) {
            auto it = map.find(id);
            if (it != map.end()) {
                page.data.push_back(std::move(it->second));
            }
        }

        return page;
    }

    bool acceptFriendRequest(const std::string &targetUserId, const std::string &cookie, std::string *outResponse) {
        if (!canUseCookie(cookie)) {
            if (outResponse) {
                *outResponse = "Banned/warned cookie";
            }
            return false;
        }

        std::string url = "https://friends.roblox.com/v1/users/" + targetUserId + "/accept-friend-request";

        auto resp = authenticatedPost(url, cookie);

        if (outResponse) {
            *outResponse = resp.text;
        }
        return resp.status_code >= 200 && resp.status_code < 300;
    }

    SocialActionResult acceptFriendRequestResult(const std::string &targetUserId, const std::string &cookie) {
        ApiError validationError = validateCookieForRequest(cookie);
        if (validationError != ApiError::Success) {
            return {false, std::string(apiErrorToString(validationError)), validationError};
        }

        std::string url = "https://friends.roblox.com/v1/users/" + targetUserId + "/accept-friend-request";
        auto resp = authenticatedPost(url, cookie);

        if (resp.status_code >= 200 && resp.status_code < 300) {
            return {true, "Friend request accepted", ApiError::Success};
        }

        return {false, resp.text, httpStatusToError(resp.status_code)};
    }

    bool sendFriendRequest(const std::string &targetUserId, const std::string &cookie, std::string *outResponse) {
        if (!canUseCookie(cookie)) {
            if (outResponse) {
                *outResponse = "Banned/warned cookie";
            }
            return false;
        }

        std::string url = "https://friends.roblox.com/v1/users/" + targetUserId + "/request-friendship";

        nlohmann::json body = {
            {"friendshipOriginSourceType", 0}
        };

        auto resp = authenticatedPost(url, cookie, body.dump());

        if (outResponse) {
            *outResponse = resp.text;
        }

        if (resp.status_code < 200 || resp.status_code >= 300) {
            LOG_ERROR("Friend request failed HTTP {}: {}", resp.status_code, resp.text);
            return false;
        }

        auto j = HttpClient::decode(resp);
        bool success = j.value("success", false);
        if (success) {
            LOG_INFO("Friend request success: {}", resp.text);
        } else {
            LOG_ERROR("Friend request API failure: {}", resp.text);
        }
        return success;
    }

    SocialActionResult sendFriendRequestResult(const std::string &targetUserId, const std::string &cookie) {
        ApiError validationError = validateCookieForRequest(cookie);
        if (validationError != ApiError::Success) {
            return {false, std::string(apiErrorToString(validationError)), validationError};
        }

        std::string url = "https://friends.roblox.com/v1/users/" + targetUserId + "/request-friendship";

        nlohmann::json body = {
            {"friendshipOriginSourceType", 0}
        };

        auto resp = authenticatedPost(url, cookie, body.dump());

        if (resp.status_code < 200 || resp.status_code >= 300) {
            return {false, resp.text, httpStatusToError(resp.status_code)};
        }

        auto j = HttpClient::decode(resp);
        bool success = j.value("success", false);
        return {success, resp.text, success ? ApiError::Success : ApiError::Unknown};
    }

    bool unfriend(const std::string &targetUserId, const std::string &cookie, std::string *outResponse) {
        if (!canUseCookie(cookie)) {
            if (outResponse) {
                *outResponse = "Banned/warned cookie";
            }
            return false;
        }

        std::string url = "https://friends.roblox.com/v1/users/" + targetUserId + "/unfriend";
        auto resp = authenticatedPost(url, cookie);

        if (outResponse) {
            *outResponse = resp.text;
        }

        if (resp.status_code < 200 || resp.status_code >= 300) {
            LOG_ERROR("Unfriend failed HTTP {}: {}", resp.status_code, resp.text);
            return false;
        }

        return true;
    }

    SocialActionResult unfriendResult(const std::string &targetUserId, const std::string &cookie) {
        ApiError validationError = validateCookieForRequest(cookie);
        if (validationError != ApiError::Success) {
            return {false, std::string(apiErrorToString(validationError)), validationError};
        }

        std::string url = "https://friends.roblox.com/v1/users/" + targetUserId + "/unfriend";
        auto resp = authenticatedPost(url, cookie);

        if (resp.status_code >= 200 && resp.status_code < 300) {
            return {true, "Unfriended successfully", ApiError::Success};
        }

        return {false, resp.text, httpStatusToError(resp.status_code)};
    }

    bool followUser(const std::string &targetUserId, const std::string &cookie, std::string *outResponse) {
        if (!canUseCookie(cookie)) {
            if (outResponse) {
                *outResponse = "Banned/warned cookie";
            }
            return false;
        }

        std::string url = "https://friends.roblox.com/v1/users/" + targetUserId + "/follow";
        auto resp = authenticatedPost(url, cookie);

        if (outResponse) {
            *outResponse = resp.text;
        }
        return resp.status_code >= 200 && resp.status_code < 300;
    }

    SocialActionResult followUserResult(const std::string &targetUserId, const std::string &cookie) {
        ApiError validationError = validateCookieForRequest(cookie);
        if (validationError != ApiError::Success) {
            return {false, std::string(apiErrorToString(validationError)), validationError};
        }

        std::string url = "https://friends.roblox.com/v1/users/" + targetUserId + "/follow";
        auto resp = authenticatedPost(url, cookie);

        if (resp.status_code >= 200 && resp.status_code < 300) {
            return {true, "Followed successfully", ApiError::Success};
        }

        return {false, resp.text, httpStatusToError(resp.status_code)};
    }

    bool unfollowUser(const std::string &targetUserId, const std::string &cookie, std::string *outResponse) {
        if (!canUseCookie(cookie)) {
            if (outResponse) {
                *outResponse = "Banned/warned cookie";
            }
            return false;
        }

        std::string url = "https://friends.roblox.com/v1/users/" + targetUserId + "/unfollow";
        auto resp = authenticatedPost(url, cookie);

        if (outResponse) {
            *outResponse = resp.text;
        }
        return resp.status_code >= 200 && resp.status_code < 300;
    }

    SocialActionResult unfollowUserResult(const std::string &targetUserId, const std::string &cookie) {
        ApiError validationError = validateCookieForRequest(cookie);
        if (validationError != ApiError::Success) {
            return {false, std::string(apiErrorToString(validationError)), validationError};
        }

        std::string url = "https://friends.roblox.com/v1/users/" + targetUserId + "/unfollow";
        auto resp = authenticatedPost(url, cookie);

        if (resp.status_code >= 200 && resp.status_code < 300) {
            return {true, "Unfollowed successfully", ApiError::Success};
        }

        return {false, resp.text, httpStatusToError(resp.status_code)};
    }

    bool blockUser(const std::string &targetUserId, const std::string &cookie, std::string *outResponse) {
        if (!canUseCookie(cookie)) {
            if (outResponse) {
                *outResponse = "Banned/warned cookie";
            }
            return false;
        }

        std::string url = "https://www.roblox.com/users/" + targetUserId + "/block";
        auto resp = authenticatedPost(url, cookie);

        if (outResponse) {
            *outResponse = resp.text;
        }
        return resp.status_code >= 200 && resp.status_code < 300;
    }

    SocialActionResult blockUserResult(const std::string &targetUserId, const std::string &cookie) {
        ApiError validationError = validateCookieForRequest(cookie);
        if (validationError != ApiError::Success) {
            return {false, std::string(apiErrorToString(validationError)), validationError};
        }

        std::string url = "https://www.roblox.com/users/" + targetUserId + "/block";
        auto resp = authenticatedPost(url, cookie);

        if (resp.status_code >= 200 && resp.status_code < 300) {
            return {true, "Blocked successfully", ApiError::Success};
        }

        return {false, resp.text, httpStatusToError(resp.status_code)};
    }

} // namespace Roblox
