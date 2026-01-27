#pragma once

#include <cstdint>
#include <ctime>
#include <expected>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.h"

namespace Roblox {

    struct VoiceSettings {
            std::string status;
            time_t bannedUntil = 0;
    };

    struct PresenceData {
            std::string presence;
            std::string lastLocation;
            uint64_t placeId = 0;
            std::string jobId;
    };

    // Get presence for a single user (uses TTL cache, 1 minute expiry)
    std::string getPresence(const std::string &cookie, uint64_t userId);

    // Get presence with full data (uses TTL cache, 1 minute expiry)
    ApiResult<PresenceData> getPresenceData(const std::string &cookie, uint64_t userId);

    // Get presence for multiple users in a single request
    // More efficient than calling getPresence multiple times
    std::unordered_map<uint64_t, PresenceData>
    getPresences(const std::vector<uint64_t> &userIds, const std::string &cookie);

    // Same as above but with ApiResult return type
    ApiResult<std::unordered_map<uint64_t, PresenceData>>
    getPresencesBatch(const std::vector<uint64_t> &userIds, const std::string &cookie);

    VoiceSettings getVoiceChatStatus(const std::string &cookie);

    void clearPresenceCache();

    void invalidatePresenceCache(uint64_t userId);

} // namespace Roblox
