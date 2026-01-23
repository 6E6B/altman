#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

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

    std::string getPresence(const std::string &cookie, uint64_t userId);

    VoiceSettings getVoiceChatStatus(const std::string &cookie);

    std::unordered_map<uint64_t, PresenceData>
    getPresences(const std::vector<uint64_t> &userIds, const std::string &cookie);

} // namespace Roblox
