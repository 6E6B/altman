#pragma once

#include <iostream>
#include <string>
#include <unordered_map>
#include <mutex>
#include <nlohmann/json.hpp>

#include "http.hpp"
#include "core/logging.hpp"
#include "core/time_utils.h"
#include "status.h"


namespace Roblox {
	enum class BanCheckResult {
		InvalidCookie,
		Unbanned,
		Banned,
		Warned,
		Terminated
	};

	struct BanInfo {
		BanCheckResult status = BanCheckResult::InvalidCookie;
		time_t endDate = 0;
		uint64_t punishedUserId = 0;  // Extract user ID from moderation response
	};

	// Cache for ban check results so we don't hit the endpoint repeatedly.
	inline std::mutex g_banStatusMutex;
	inline std::unordered_map<std::string, BanCheckResult> g_banStatusCache;

	static BanInfo checkBanStatus(const std::string &cookie) {
		LOG_INFO("Checking moderation status");
		HttpClient::Response response = HttpClient::get(
			"https://usermoderation.roblox.com/v1/not-approved",
			{{"Cookie", ".ROBLOSECURITY=" + cookie}});

		if (response.status_code < 200 || response.status_code >= 300) {
			LOG_ERROR("Failed moderation check: HTTP " + std::to_string(response.status_code));
			return {BanCheckResult::InvalidCookie, 0};
		}

		auto j = HttpClient::decode(response);
		if (j.is_object() && j.contains("punishmentTypeDescription")) {
			std::string punishmentType = j["punishmentTypeDescription"].get<std::string>();
			time_t end = 0;
			uint64_t punishedUserId = j.value("punishedUserId", 0ULL);
			bool hasEndDate = j.contains("endDate") && j["endDate"].is_string() && !j["endDate"].get<std::string>().
			                  empty();

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
		if (j.empty())
			return {BanCheckResult::Unbanned, 0};
		return {BanCheckResult::Unbanned, 0};
	}

	static BanCheckResult cachedBanStatus(const std::string &cookie) { {
			std::lock_guard<std::mutex> lock(g_banStatusMutex);
			auto it = g_banStatusCache.find(cookie);
			if (it != g_banStatusCache.end())
				return it->second;
		}

		BanCheckResult status = checkBanStatus(cookie).status; {
			std::lock_guard<std::mutex> lock(g_banStatusMutex);
			g_banStatusCache[cookie] = status;
		}
		return status;
	}

	// Force refresh the cached ban status for a cookie
	static BanCheckResult refreshBanStatus(const std::string &cookie) {
		BanCheckResult status = checkBanStatus(cookie).status; {
			std::lock_guard<std::mutex> lock(g_banStatusMutex);
			g_banStatusCache[cookie] = status;
		}
		return status;
	}


	static bool isCookieValid(const std::string &cookie) {
		return cachedBanStatus(cookie) != BanCheckResult::InvalidCookie;
	}

	static bool canUseCookie(const std::string &cookie) {
		BanCheckResult status = cachedBanStatus(cookie);
		if (status == BanCheckResult::Banned) {
			LOG_ERROR("Skipping request: cookie is banned");
			return false;
		}
		if (status == BanCheckResult::Warned) {
			LOG_ERROR("Skipping request: cookie is warned");
			return false;
		}
		if (status == BanCheckResult::Terminated) {
			LOG_ERROR("Skipping request: cookie is terminated");
			return false;
		}
		if (status == BanCheckResult::InvalidCookie) {
			LOG_ERROR("Skipping request: invalid cookie");
			return false;
		}
		return true;
	}

	static nlohmann::json getAuthenticatedUser(const std::string &cookie) {
		if (!canUseCookie(cookie))
			return nlohmann::json::object();

		LOG_INFO("Fetching profile info");
		HttpClient::Response response = HttpClient::get(
			"https://users.roblox.com/v1/users/authenticated",
			{{"Cookie", ".ROBLOSECURITY=" + cookie}});

		if (response.status_code < 200 || response.status_code >= 300) {
			LOG_ERROR("Failed to fetch user info: HTTP " + std::to_string(response.status_code));
			return nlohmann::json::object();
		}

		return HttpClient::decode(response);
	}


	static std::string fetchAuthTicket(const std::string &cookie) {
		if (!canUseCookie(cookie))
			return "";
		LOG_INFO("Fetching x-csrf token");
		std::cout << cookie;
		auto csrfResponse = HttpClient::post(
			"https://auth.roblox.com/v1/authentication-ticket",
			{{"Cookie", ".ROBLOSECURITY=" + cookie}});

		auto csrfToken = csrfResponse.headers.find("x-csrf-token");
		if (csrfToken == csrfResponse.headers.end()) {
			std::cerr << "failed to get CSRF token\n";

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

		if (ticketResponse.status_code < 200 || ticketResponse.status_code >= 300) {
			LOG_ERROR("Failed to fetch auth ticket: HTTP " + std::to_string(ticketResponse.status_code));
			return "";
		}

		auto ticket = ticketResponse.headers.find("rbx-authentication-ticket");
		if (ticket == ticketResponse.headers.end()) {
			std::cerr << "failed to get authentication ticket\n";
			LOG_INFO("Failed to get authentication ticket");
			return "";
		}

		return ticket->second;
	}

	static uint64_t getUserId(const std::string &cookie) {
		auto userJson = getAuthenticatedUser(cookie);
		return userJson.value("id", 0ULL);
	}

	static std::string getUsername(const std::string &cookie) {
		auto userJson = getAuthenticatedUser(cookie);
		return userJson.value("name", "");
	}

	static std::string getDisplayName(const std::string &cookie) {
		auto userJson = getAuthenticatedUser(cookie);
		return userJson.value("displayName", "");
	}
}
