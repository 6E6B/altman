#pragma once

#include <iostream>
#include <string>
#include <nlohmann/json.hpp>

#include "http.hpp"
#include "core/logging.hpp"
#include "status.h"

using namespace std;

namespace RobloxApi {
        enum class BanCheckResult {
                InvalidCookie,
                Unbanned,
                Banned
        };

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
}

