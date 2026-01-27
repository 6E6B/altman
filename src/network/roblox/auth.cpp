#include "auth.h"

#include <format>
#include <future>

#include "common.h"
#include "console/console.h"
#include "network/http.h"
#include "session.h"
#include "utils/time_utils.h"

namespace Roblox {

    namespace {
        // TTL Cache for ban status (30 minutes)
        TtlCache<std::string, BanInfo> g_banCache {std::chrono::minutes(30)};

        // TTL Cache for authenticated user info (1 hour)
        TtlCache<std::string, AuthenticatedUserInfo> g_userInfoCache {std::chrono::hours(1)};
    } // namespace

    BanInfo checkBanStatus(const std::string &cookie) {
        LOG_INFO("Checking moderation status");

        HttpClient::Response response = HttpClient::rateLimitedGet(
            "https://usermoderation.roblox.com/v1/not-approved",
            {
                {"Cookie", ".ROBLOSECURITY=" + cookie}
            }
        );

        if (response.status_code < 200 || response.status_code >= 300) {
            LOG_ERROR("Failed moderation check: HTTP {}", response.status_code);

            if (response.status_code == 401 || response.status_code == 403) {
                return {BanCheckResult::InvalidCookie, 0, 0};
            }

            if (response.status_code == 429) {
                HttpClient::RateLimiter::instance().backoff(std::chrono::seconds(2));
                return {BanCheckResult::NetworkError, 0, 0};
            }

            return {BanCheckResult::NetworkError, 0, 0};
        }

        auto j = HttpClient::decode(response);

        if (j.is_object() && j.contains("punishmentTypeDescription")) {
            std::string punishmentType = j["punishmentTypeDescription"].get<std::string>();
            time_t end = 0;
            uint64_t punishedUserId = j.value("punishedUserId", 0ULL);
            bool hasEndDate = j.contains("endDate") && j["endDate"].is_string()
                              && !j["endDate"].get<std::string>().empty();

            if (hasEndDate) {
                end = parseIsoTimestamp(j["endDate"].get<std::string>());
                return {BanCheckResult::Banned, end, punishedUserId};
            }

            if (punishmentType == "Delete") {
                return {BanCheckResult::Terminated, 0, punishedUserId};
            }

            if (punishmentType == "Warn") {
                return {BanCheckResult::Warned, 0, punishedUserId};
            }

            // Default to banned for other punishment types without end date
            return {BanCheckResult::Banned, 0, punishedUserId};
        }

        return {BanCheckResult::Unbanned, 0, 0};
    }

    BanCheckResult cachedBanStatus(const std::string &cookie) {
        if (auto cached = g_banCache.get(cookie)) {
            return cached->status;
        }

        BanInfo info = checkBanStatus(cookie);
        g_banCache.set(cookie, info);

        return info.status;
    }

    BanCheckResult refreshBanStatus(const std::string &cookie) {
        g_banCache.invalidate(cookie);
        BanInfo info = checkBanStatus(cookie);
        g_banCache.set(cookie, info);
        return info.status;
    }

    bool isCookieValid(const std::string &cookie) {
        return cachedBanStatus(cookie) != BanCheckResult::InvalidCookie;
    }

    bool canUseCookie(const std::string &cookie) {
        BanCheckResult status = cachedBanStatus(cookie);

        switch (status) {
            case BanCheckResult::Banned:
                LOG_ERROR("Skipping request: cookie is banned");
                return false;
            case BanCheckResult::Warned:
                LOG_ERROR("Skipping request: cookie is warned");
                return false;
            case BanCheckResult::Terminated:
                LOG_ERROR("Skipping request: cookie is terminated");
                return false;
            case BanCheckResult::InvalidCookie:
                LOG_ERROR("Skipping request: invalid cookie");
                return false;
            case BanCheckResult::NetworkError:
                LOG_ERROR("Skipping request: network error during ban check");
                return false;
            case BanCheckResult::Unbanned:
            default:
                return true;
        }
    }

    nlohmann::json getAuthenticatedUser(const std::string &cookie) {
        if (!canUseCookie(cookie)) {
            return nlohmann::json::object();
        }

        if (auto cached = g_userInfoCache.get(cookie)) {
            return nlohmann::json {
                {"id",          cached->userId     },
                {"name",        cached->username   },
                {"displayName", cached->displayName}
            };
        }

        LOG_INFO("Fetching profile info");

        HttpClient::Response response = HttpClient::rateLimitedGet(
            "https://users.roblox.com/v1/users/authenticated",
            {
                {"Cookie", ".ROBLOSECURITY=" + cookie}
            }
        );

        if (response.status_code < 200 || response.status_code >= 300) {
            LOG_ERROR("Failed to fetch user info: HTTP {}", response.status_code);
            return nlohmann::json::object();
        }

        auto j = HttpClient::decode(response);

        if (j.is_object() && j.contains("id")) {
            AuthenticatedUserInfo info;
            info.userId = j.value("id", 0ULL);
            info.username = j.value("name", "");
            info.displayName = j.value("displayName", "");
            g_userInfoCache.set(cookie, info);
        }

        return j;
    }

    ApiResult<AuthenticatedUserInfo> getAuthenticatedUserInfo(const std::string &cookie) {
        ApiError validationError = validateCookieForRequest(cookie);
        if (validationError != ApiError::Success) {
            return std::unexpected(validationError);
        }

        if (auto cached = g_userInfoCache.get(cookie)) {
            return *cached;
        }

        LOG_INFO("Fetching profile info");

        HttpClient::Response response = HttpClient::rateLimitedGet(
            "https://users.roblox.com/v1/users/authenticated",
            {
                {"Cookie", ".ROBLOSECURITY=" + cookie}
            }
        );

        if (response.status_code < 200 || response.status_code >= 300) {
            LOG_ERROR("Failed to fetch user info: HTTP {}", response.status_code);
            return std::unexpected(httpStatusToError(response.status_code));
        }

        auto j = HttpClient::decode(response);

        if (!j.is_object() || !j.contains("id")) {
            return std::unexpected(ApiError::InvalidResponse);
        }

        AuthenticatedUserInfo info;
        info.userId = j.value("id", 0ULL);
        info.username = j.value("name", "");
        info.displayName = j.value("displayName", "");

        g_userInfoCache.set(cookie, info);

        return info;
    }

    ApiResult<FullAccountInfo> fetchFullAccountInfo(const std::string &cookie) {
        BanInfo banInfo = checkBanStatus(cookie);
        g_banCache.set(cookie, banInfo);

        if (banInfo.status == BanCheckResult::InvalidCookie) {
            return std::unexpected(ApiError::InvalidCookie);
        }

        FullAccountInfo result;
        result.banInfo = banInfo;

        if (banInfo.status == BanCheckResult::Unbanned) {
            HttpClient::Response userResponse = HttpClient::rateLimitedGet(
                "https://users.roblox.com/v1/users/authenticated",
                {
                    {"Cookie", ".ROBLOSECURITY=" + cookie}
                }
            );

            if (userResponse.status_code >= 200 && userResponse.status_code < 300) {
                auto userJson = HttpClient::decode(userResponse);
                if (userJson.is_object()) {
                    result.userId = userJson.value("id", 0ULL);
                    result.username = userJson.value("name", "");
                    result.displayName = userJson.value("displayName", "");

                    AuthenticatedUserInfo userInfo {result.userId, result.username, result.displayName};
                    g_userInfoCache.set(cookie, userInfo);
                }
            }

            auto presenceFuture = std::async(std::launch::async, [&]() {
                return getPresence(cookie, result.userId);
            });

            result.voiceSettings = getVoiceChatStatus(cookie);
            result.presence = presenceFuture.get();
        } else {
            result.presence = std::string(banResultToString(banInfo.status));
            result.voiceSettings = {"N/A", 0};
        }

        return result;
    }

    uint64_t getUserId(const std::string &cookie) {
        auto result = getAuthenticatedUserInfo(cookie);
        return result ? result->userId : 0;
    }

    std::string getUsername(const std::string &cookie) {
        auto result = getAuthenticatedUserInfo(cookie);
        return result ? result->username : "";
    }

    std::string getDisplayName(const std::string &cookie) {
        auto result = getAuthenticatedUserInfo(cookie);
        return result ? result->displayName : "";
    }

    std::string fetchAuthTicket(const std::string &cookie) {
        if (!canUseCookie(cookie)) {
            return "";
        }

        LOG_INFO("Fetching authentication ticket");

        auto response = authenticatedPost("https://auth.roblox.com/v1/authentication-ticket", cookie);

        if (response.status_code < 200 || response.status_code >= 300) {
            LOG_ERROR("Failed to fetch auth ticket: HTTP {}", response.status_code);
            return "";
        }

        auto ticket = response.headers.find("rbx-authentication-ticket");
        if (ticket == response.headers.end()) {
            LOG_ERROR("Failed to get authentication ticket from response headers");
            return "";
        }

        return ticket->second;
    }

    void clearAuthCaches() {
        g_banCache.clear();
        g_userInfoCache.clear();
        CsrfManager::instance().clear();
    }

    void invalidateCacheForCookie(const std::string &cookie) {
        g_banCache.invalidate(cookie);
        g_userInfoCache.invalidate(cookie);
        CsrfManager::instance().invalidateToken(cookie);
    }

} // namespace Roblox
