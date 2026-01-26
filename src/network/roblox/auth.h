#pragma once

#include <cstdint>
#include <ctime>
#include <expected>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "common.h"
#include "session.h"

namespace Roblox {

    enum class BanCheckResult {
        NetworkError,
        InvalidCookie,
        Unbanned,
        Banned,
        Warned,
        Terminated
    };

    constexpr std::string_view banResultToString(BanCheckResult result) noexcept {
        switch (result) {
            case BanCheckResult::NetworkError:
                return "NetworkError";
            case BanCheckResult::InvalidCookie:
                return "InvalidCookie";
            case BanCheckResult::Unbanned:
                return "Unbanned";
            case BanCheckResult::Banned:
                return "Banned";
            case BanCheckResult::Warned:
                return "Warned";
            case BanCheckResult::Terminated:
                return "Terminated";
            default:
                return "Unknown";
        }
    }

    struct BanInfo {
            BanCheckResult status = BanCheckResult::NetworkError;
            time_t endDate = 0;
            uint64_t punishedUserId = 0;
    };

    struct AuthenticatedUserInfo {
            uint64_t userId = 0;
            std::string username;
            std::string displayName;
    };

    struct FullAccountInfo {
            uint64_t userId = 0;
            std::string username;
            std::string displayName;

            BanInfo banInfo;

            std::string presence;
            VoiceSettings voiceSettings;
    };

    // Fetch ban status directly from API (no caching)
    BanInfo checkBanStatus(const std::string &cookie);

    // Get cached ban status (uses TTL cache, 30 minute expiry)
    BanCheckResult cachedBanStatus(const std::string &cookie);

    // Refresh ban status (invalidates cache and re-fetches)
    BanCheckResult refreshBanStatus(const std::string &cookie);

    bool isCookieValid(const std::string &cookie);
    bool canUseCookie(const std::string &cookie);

    ApiResult<AuthenticatedUserInfo> getAuthenticatedUserInfo(const std::string &cookie);

    ApiResult<FullAccountInfo> fetchFullAccountInfo(const std::string &cookie);

    uint64_t getUserId(const std::string &cookie);
    std::string getUsername(const std::string &cookie);
    std::string getDisplayName(const std::string &cookie);

    std::string fetchAuthTicket(const std::string &cookie);

    void clearAuthCaches();

    void invalidateCacheForCookie(const std::string &cookie);

} // namespace Roblox
