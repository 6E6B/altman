#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

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

std::vector<FriendInfo> getFriends(const std::string& userId, const std::string& cookie);

FriendInfo getUserInfo(const std::string& userId);

FriendDetail getUserDetails(const std::string& userId, const std::string& cookie);

FriendRequestsPage getIncomingFriendRequests(
    const std::string& cookie,
    const std::string& cursor = {},
    int limit = 100
);

bool acceptFriendRequest(
    const std::string& targetUserId,
    const std::string& cookie,
    std::string* outResponse = nullptr
);

bool sendFriendRequest(
    const std::string& targetUserId,
    const std::string& cookie,
    std::string* outResponse = nullptr
);

uint64_t getUserIdFromUsername(const std::string& username);

bool unfriend(
    const std::string& targetUserId,
    const std::string& cookie,
    std::string* outResponse = nullptr
);

bool followUser(
    const std::string& targetUserId,
    const std::string& cookie,
    std::string* outResponse = nullptr
);

bool unfollowUser(
    const std::string& targetUserId,
    const std::string& cookie,
    std::string* outResponse = nullptr
);

bool blockUser(
    const std::string& targetUserId,
    const std::string& cookie,
    std::string* outResponse = nullptr
);

}