#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.h"

struct FriendInfo;

namespace Roblox {

    struct FriendDetail {
            uint64_t id = 0;
            std::string username;
            std::string displayName;
            std::string description;
            std::string createdIso;
            int friends = 0;
            int followers = 0;
            int following = 0;
            int placeVisits = 0;
            std::string presence;
    };

    struct IncomingFriendRequest {
            uint64_t userId = 0;
            std::string username;
            std::string displayName;
            std::string sentAt;
            std::vector<std::string> mutuals;
            std::string originSourceType;
            uint64_t sourceUniverseId = 0;
    };

    struct FriendRequestsPage {
            std::vector<IncomingFriendRequest> data;
            std::string nextCursor;
            std::string prevCursor;
    };

    struct SocialActionResult {
            bool success = false;
            std::string message;
            ApiError error = ApiError::Success;
    };

    std::vector<FriendInfo> getFriends(const std::string &userId, const std::string &cookie);

    ApiResult<std::vector<FriendInfo>> getFriendsList(const std::string &userId, const std::string &cookie);

    // Get basic user info (does not require authentication)
    FriendInfo getUserInfo(const std::string &userId);

    ApiResult<FriendInfo> getUserInfoResult(const std::string &userId);

    FriendDetail getUserDetails(const std::string &userId, const std::string &cookie);

    ApiResult<FriendDetail> getUserDetailsResult(const std::string &userId, const std::string &cookie);

    uint64_t getUserIdFromUsername(const std::string &username);

    ApiResult<uint64_t> getUserIdFromUsernameResult(const std::string &username);

    FriendRequestsPage
    getIncomingFriendRequests(const std::string &cookie, const std::string &cursor = {}, int limit = 100);

    bool acceptFriendRequest(
        const std::string &targetUserId,
        const std::string &cookie,
        std::string *outResponse = nullptr
    );

    SocialActionResult acceptFriendRequestResult(const std::string &targetUserId, const std::string &cookie);

    bool sendFriendRequest(
        const std::string &targetUserId,
        const std::string &cookie,
        std::string *outResponse = nullptr
    );

    SocialActionResult sendFriendRequestResult(const std::string &targetUserId, const std::string &cookie);

    bool unfriend(const std::string &targetUserId, const std::string &cookie, std::string *outResponse = nullptr);

    SocialActionResult unfriendResult(const std::string &targetUserId, const std::string &cookie);

    bool followUser(const std::string &targetUserId, const std::string &cookie, std::string *outResponse = nullptr);

    SocialActionResult followUserResult(const std::string &targetUserId, const std::string &cookie);

    bool unfollowUser(const std::string &targetUserId, const std::string &cookie, std::string *outResponse = nullptr);

    SocialActionResult unfollowUserResult(const std::string &targetUserId, const std::string &cookie);

    bool blockUser(const std::string &targetUserId, const std::string &cookie, std::string *outResponse = nullptr);

    SocialActionResult blockUserResult(const std::string &targetUserId, const std::string &cookie);

} // namespace Roblox
