#pragma once

#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "network/http.h"

struct ImVec4;

namespace Roblox {

    enum class ApiError {
        Success = 0,

        NetworkError,
        Timeout,
        ConnectionFailed,

        InvalidCookie,
        CookieBanned,
        CookieWarned,
        CookieTerminated,
        CsrfTokenMissing,
        Unauthorized,

        RateLimited,

        NotFound,
        ParseError,
        InvalidResponse,
        InvalidInput,

        Unknown
    };

    constexpr std::string_view apiErrorToString(ApiError error) noexcept {
        switch (error) {
            case ApiError::Success:
                return "Success";
            case ApiError::NetworkError:
                return "Network error";
            case ApiError::Timeout:
                return "Request timed out";
            case ApiError::ConnectionFailed:
                return "Connection failed";
            case ApiError::InvalidCookie:
                return "Invalid or expired cookie";
            case ApiError::CookieBanned:
                return "Account is banned";
            case ApiError::CookieWarned:
                return "Account has active warning";
            case ApiError::CookieTerminated:
                return "Account is terminated";
            case ApiError::CsrfTokenMissing:
                return "Failed to obtain CSRF token";
            case ApiError::Unauthorized:
                return "Unauthorized";
            case ApiError::RateLimited:
                return "Rate limited";
            case ApiError::NotFound:
                return "Not found";
            case ApiError::ParseError:
                return "Failed to parse response";
            case ApiError::InvalidResponse:
                return "Invalid response from server";
            case ApiError::InvalidInput:
                return "Invalid input";
            case ApiError::Unknown:
            default:
                return "Unknown error";
        }
    }

    constexpr bool isRetryableError(ApiError error) noexcept {
        switch (error) {
            case ApiError::NetworkError:
            case ApiError::Timeout:
            case ApiError::ConnectionFailed:
            case ApiError::RateLimited:
                return true;
            default:
                return false;
        }
    }

    inline ApiError httpStatusToError(int statusCode) {
        if (statusCode >= 200 && statusCode < 300) {
            return ApiError::Success;
        }
        if (statusCode == 401) {
            return ApiError::InvalidCookie;
        }
        if (statusCode == 403) {
            return ApiError::Unauthorized;
        }
        if (statusCode == 404) {
            return ApiError::NotFound;
        }
        if (statusCode == 429) {
            return ApiError::RateLimited;
        }
        if (statusCode >= 500) {
            return ApiError::NetworkError;
        }
        return ApiError::Unknown;
    }

    template <typename T>
    using ApiResult = std::expected<T, ApiError>;

    template <typename Key, typename Value>
    class TtlCache {
        public:
            using Clock = std::chrono::steady_clock;
            using Duration = std::chrono::seconds;

            explicit TtlCache(Duration defaultTtl) : m_defaultTtl(defaultTtl) {}

            std::optional<Value> get(const Key &key) const {
                std::shared_lock lock(m_mutex);
                auto it = m_cache.find(key);
                if (it == m_cache.end()) {
                    return std::nullopt;
                }
                if (Clock::now() > it->second.expiresAt) {
                    return std::nullopt;
                }
                return it->second.value;
            }

            void set(const Key &key, Value value, std::optional<Duration> ttl = std::nullopt) {
                std::unique_lock lock(m_mutex);
                m_cache[key] = Entry {
                    .value = std::move(value),
                    .expiresAt = Clock::now() + ttl.value_or(m_defaultTtl)
                };
            }

            void invalidate(const Key &key) {
                std::unique_lock lock(m_mutex);
                m_cache.erase(key);
            }

            void clear() {
                std::unique_lock lock(m_mutex);
                m_cache.clear();
            }

            void prune() {
                std::unique_lock lock(m_mutex);
                auto now = Clock::now();
                std::erase_if(m_cache, [now](const auto &pair) {
                    return now > pair.second.expiresAt;
                });
            }

            size_t size() const {
                std::shared_lock lock(m_mutex);
                return m_cache.size();
            }

        private:
            struct Entry {
                    Value value;
                    Clock::time_point expiresAt;
            };

            Duration m_defaultTtl;
            mutable std::shared_mutex m_mutex;
            std::unordered_map<Key, Entry> m_cache;
    };

    class CsrfManager {
        public:
            static CsrfManager &instance();

            std::string getToken(const std::string &cookie) const;

            void updateToken(const std::string &cookie, const std::string &token);

            void invalidateToken(const std::string &cookie);

            void clear();

        private:
            CsrfManager() = default;

            mutable std::shared_mutex m_mutex;
            std::unordered_map<std::string, std::string> m_tokens;
    };

    inline std::vector<std::pair<std::string, std::string>>
    makeAuthHeaders(const std::string &cookie, const std::string &csrf = {}) {
        std::vector<std::pair<std::string, std::string>> headers = {
            {"Cookie", ".ROBLOSECURITY=" + cookie},
            {"Accept", "application/json"        }
        };

        if (!csrf.empty()) {
            headers.emplace_back("X-CSRF-TOKEN", csrf);
            headers.emplace_back("Origin", "https://www.roblox.com");
            headers.emplace_back("Referer", "https://www.roblox.com/");
        }

        return headers;
    }

    HttpClient::Response authenticatedPost(
        const std::string &url,
        const std::string &cookie,
        const std::string &jsonBody = {},
        std::initializer_list<std::pair<const std::string, std::string>> extraHeaders = {}
    );

    enum class BanCheckResult;
    BanCheckResult cachedBanStatus(const std::string &cookie);

    // Check if cookie is usable for requests (not banned/warned/terminated/invalid)
    // Returns the specific error if not usable, or Success if usable
    ApiError validateCookieForRequest(const std::string &cookie);

    inline bool jsonToString(const nlohmann::json &j, std::string &out) {
        if (j.is_string()) {
            out = j.get_ref<const std::string &>();
            return true;
        }
        if (j.is_number_integer()) {
            out = std::to_string(j.get<int64_t>());
            return true;
        }
        if (j.is_number_unsigned()) {
            out = std::to_string(j.get<uint64_t>());
            return true;
        }
        return false;
    }

    inline bool jsonToU64(const nlohmann::json &j, uint64_t &out) {
        if (j.is_number_unsigned()) {
            out = j.get<uint64_t>();
            return true;
        }
        if (j.is_number_integer()) {
            int64_t v = j.get<int64_t>();
            if (v >= 0) {
                out = static_cast<uint64_t>(v);
                return true;
            }
        }
        if (j.is_string()) {
            const auto &s = j.get_ref<const std::string &>();
            uint64_t v = 0;
            auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
            if (ec == std::errc {}) {
                out = v;
                return true;
            }
        }
        return false;
    }

} // namespace Roblox

ImVec4 getStatusColor(std::string statusCode);

std::string generateSessionId();
std::string presenceTypeToString(int type);

struct UserSpecifier {
        bool isId = false;
        uint64_t id = 0;
        std::string username;
};

constexpr std::string_view trim_view(std::string_view s) noexcept {
    auto is_space = [](unsigned char c) {
        return static_cast<bool>(std::isspace(c));
    };

    while (!s.empty() && is_space(s.front())) {
        s.remove_prefix(1);
    }

    while (!s.empty() && is_space(s.back())) {
        s.remove_suffix(1);
    }

    return s;
}

bool parseUserSpecifier(std::string_view raw, UserSpecifier &out);

struct PublicServerInfo {
        std::string jobId;
        int currentPlayers = 0;
        int maximumPlayers = 0;
        double averagePing = 0.0;
        double averageFps = 0.0;
};

struct GameInfo {
        std::string name;
        uint64_t universeId = 0;
        uint64_t placeId = 0;
        int playerCount = 0;
        int upVotes = 0;
        int downVotes = 0;
        std::string creatorName;
        bool creatorVerified = false;
};