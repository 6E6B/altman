#include "common.h"

#include <cctype>
#include <random>

#include <imgui.h>

#include "auth.h"
#include "console/console.h"

namespace Roblox {

    CsrfManager &CsrfManager::instance() {
        static CsrfManager instance;
        return instance;
    }

    std::string CsrfManager::getToken(const std::string &cookie) const {
        std::shared_lock lock(m_mutex);
        auto it = m_tokens.find(cookie);
        return it != m_tokens.end() ? it->second : std::string {};
    }

    void CsrfManager::updateToken(const std::string &cookie, const std::string &token) {
        std::unique_lock lock(m_mutex);
        m_tokens[cookie] = token;
    }

    void CsrfManager::invalidateToken(const std::string &cookie) {
        std::unique_lock lock(m_mutex);
        m_tokens.erase(cookie);
    }

    void CsrfManager::clear() {
        std::unique_lock lock(m_mutex);
        m_tokens.clear();
    }

    HttpClient::Response authenticatedPost(
        const std::string &url,
        const std::string &cookie,
        const std::string &jsonBody,
        std::initializer_list<std::pair<const std::string, std::string>> extraHeaders
    ) {
        auto &csrfMgr = CsrfManager::instance();

        auto buildHeaders = [&](const std::string &csrf) {
            std::vector<std::pair<std::string, std::string>> headers = {
                {"Cookie",  ".ROBLOSECURITY=" + cookie},
                {"Accept",  "application/json"        },
                {"Origin",  "https://www.roblox.com"  },
                {"Referer", "https://www.roblox.com/" }
            };

            if (!csrf.empty()) {
                headers.emplace_back("X-CSRF-TOKEN", csrf);
            }

            if (!jsonBody.empty()) {
                headers.emplace_back("Content-Type", "application/json");
            }

            for (const auto &[key, value]: extraHeaders) {
                headers.emplace_back(key, value);
            }

            return headers;
        };

        std::string csrf = csrfMgr.getToken(cookie);
        auto headers = buildHeaders(csrf);

        HttpClient::Response resp = HttpClient::post(url, {headers.begin(), headers.end()}, jsonBody);

        if (resp.status_code == 403) {
            auto it = resp.headers.find("x-csrf-token");
            if (it != resp.headers.end() && !it->second.empty()) {
                LOG_INFO("CSRF token expired, retrying with new token");
                csrfMgr.updateToken(cookie, it->second);

                headers = buildHeaders(it->second);
                resp = HttpClient::post(url, {headers.begin(), headers.end()}, jsonBody);
            }
        }

        if (resp.status_code >= 200 && resp.status_code < 300) {
            auto it = resp.headers.find("x-csrf-token");
            if (it != resp.headers.end() && !it->second.empty()) {
                csrfMgr.updateToken(cookie, it->second);
            }
        }

        return resp;
    }

    ApiError validateCookieForRequest(const std::string &cookie) {
        if (cookie.empty()) {
            return ApiError::InvalidInput;
        }

        BanCheckResult status = cachedBanStatus(cookie);

        switch (status) {
            case BanCheckResult::Unbanned:
                return ApiError::Success;
            case BanCheckResult::InvalidCookie:
                return ApiError::InvalidCookie;
            case BanCheckResult::Banned:
                return ApiError::CookieBanned;
            case BanCheckResult::Warned:
                return ApiError::CookieWarned;
            case BanCheckResult::Terminated:
                return ApiError::CookieTerminated;
            case BanCheckResult::NetworkError:
                return ApiError::NetworkError;
            default:
                return ApiError::Unknown;
        }
    }

} // namespace Roblox

ImVec4 getStatusColor(std::string statusCode) {
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
    if (statusCode == "Warned") {
        return ImVec4(1.0f, 0.8f, 0.0f, 1.0f);
    }
    if (statusCode == "Terminated") {
        return ImVec4(0.8f, 0.1f, 0.1f, 1.0f);
    }
    if (statusCode == "InvalidCookie") {
        return ImVec4(0.9f, 0.4f, 0.9f, 1.0f);
    }
    return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
}

std::string generateSessionId() {
    static const char *hex = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);

    std::string uuid(36, ' ');
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

std::string presenceTypeToString(int type) {
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

bool parseUserSpecifier(std::string_view raw, UserSpecifier &out) {
    const std::string_view s = trim_view(raw);
    if (s.empty()) {
        return false;
    }

    // Fast path: id=NUMBER (case-insensitive)
    if (s.size() > 3 && (s[0] == 'i' || s[0] == 'I') && (s[1] == 'd' || s[1] == 'D') && s[2] == '=') {

        const std::string_view num = s.substr(3);
        if (num.empty()) {
            return false;
        }

        uint64_t value {};
        const auto [ptr, ec] = std::from_chars(num.data(), num.data() + num.size(), value);

        if (ec != std::errc {} || ptr != num.data() + num.size()) {
            return false;
        }

        out = {.isId = true, .id = value, .username = {}};
        return true;
    }

    // Username validation
    for (unsigned char ch: s) {
        if (!(std::isalnum(ch) || ch == '_')) {
            return false;
        }
    }

    out = {.isId = false, .id = 0, .username = std::string {s}};
    return true;
}