#pragma once

#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

#include "authenticated_http.h"
#include "core/logging.hpp"
#include "core/time_utils.h"
#include "hba.h"
#include "hba_client.h"
#include "http.hpp"
#include "status.h"

namespace Roblox {
	enum class BanCheckResult { InvalidCookie, Unbanned, Banned, Warned, Terminated };

	struct BanInfo {
			BanCheckResult status = BanCheckResult::InvalidCookie;
			time_t endDate = 0;
			uint64_t punishedUserId = 0; // Extract user ID from moderation response
	};

	// Cache for ban check results so we don't hit the endpoint repeatedly.
	inline std::mutex g_banStatusMutex;
	inline std::unordered_map<std::string, BanCheckResult> g_banStatusCache;

	static BanInfo checkBanStatus(const std::string &cookie) {
		LOG_INFO("Checking moderation status");
		HttpClient::Response response = HttpClient::get(
			"https://usermoderation.roblox.com/v1/not-approved",
			{
				{"Cookie", ".ROBLOSECURITY=" + cookie}
		}
		);

		if (response.status_code < 200 || response.status_code >= 300) {
			LOG_ERROR("Failed moderation check: HTTP " + std::to_string(response.status_code));
			return {BanCheckResult::InvalidCookie, 0};
		}

		auto j = HttpClient::decode(response);
		if (j.is_object() && j.contains("punishmentTypeDescription")) {
			std::string punishmentType = j["punishmentTypeDescription"].get<std::string>();
			time_t end = 0;
			uint64_t punishedUserId = j.value("punishedUserId", 0ULL);
			bool hasEndDate
				= j.contains("endDate") && j["endDate"].is_string() && !j["endDate"].get<std::string>().empty();

			if (hasEndDate) {
				end = parseIsoTimestamp(j["endDate"].get<std::string>());
				return {BanCheckResult::Banned, end, punishedUserId};
			}

			if (punishmentType == "Delete") { return {BanCheckResult::Terminated, 0, punishedUserId}; }

			if (punishmentType == "Warn") { return {BanCheckResult::Warned, 0, punishedUserId}; }

			// Default to banned for other punishment types without end date
			return {BanCheckResult::Banned, 0, punishedUserId};
		}
		if (j.empty()) { return {BanCheckResult::Unbanned, 0}; }
		return {BanCheckResult::Unbanned, 0};
	}

	static BanCheckResult cachedBanStatus(const std::string &cookie) {
		{
			std::lock_guard<std::mutex> lock(g_banStatusMutex);
			auto it = g_banStatusCache.find(cookie);
			if (it != g_banStatusCache.end()) { return it->second; }
		}

		BanCheckResult status = checkBanStatus(cookie).status;
		{
			std::lock_guard<std::mutex> lock(g_banStatusMutex);
			g_banStatusCache[cookie] = status;
		}
		return status;
	}

	// Force refresh the cached ban status for a cookie
	static BanCheckResult refreshBanStatus(const std::string &cookie) {
		BanCheckResult status = checkBanStatus(cookie).status;
		{
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

	// ============================================================================
	// HBA-enabled methods (with BAT support)
	// ============================================================================

	/**
	 * Get authenticated user info with HBA support
	 * @param config HBA authentication configuration
	 * @return JSON object with user info, or empty object on failure
	 */
	static nlohmann::json getAuthenticatedUser(const HBA::AuthConfig &config) {
		if (!canUseCookie(config.cookie)) { return nlohmann::json::object(); }

		LOG_INFO("Fetching profile info (HBA-enabled)");

		const std::string url = "https://users.roblox.com/v1/users/authenticated";
		HttpClient::Response response = AuthenticatedHttp::get(url, config);

		if (response.status_code < 200 || response.status_code >= 300) {
			LOG_ERROR("Failed to fetch user info: HTTP " + std::to_string(response.status_code));
			return nlohmann::json::object();
		}

		return HttpClient::decode(response);
	}

	/**
	 * Fetch authentication ticket with HBA support
	 * This endpoint is protected by Account Session Protection
	 * @param config HBA authentication configuration
	 * @return Authentication ticket string, or empty on failure
	 */
	static std::string fetchAuthTicket(const HBA::AuthConfig &config) {
		if (!canUseCookie(config.cookie)) { return ""; }

		const std::string url = "https://auth.roblox.com/v1/authentication-ticket";

		LOG_INFO("Fetching x-csrf token (HBA-enabled)");

		// First request to get CSRF token
		auto csrfResponse = AuthenticatedHttp::post(url, config);

		auto csrfToken = csrfResponse.headers.find("x-csrf-token");
		if (csrfToken == csrfResponse.headers.end()) {
			LOG_ERROR("Failed to get CSRF token");
			return "";
		}

		LOG_INFO("Fetching authentication ticket");

		// Build headers for the actual request
		std::map<std::string, std::string> headers;
		headers["Cookie"] = ".ROBLOSECURITY=" + config.cookie;
		headers["Origin"] = "https://www.roblox.com";
		headers["Referer"] = "https://www.roblox.com/";
		headers["X-CSRF-TOKEN"] = csrfToken->second;

		// Generate BAT header if HBA is enabled
		if (config.hasHBA()) {
			auto batHeaders = HBA::getClient().generateBATHeaders(config, url, "POST", "");
			for (const auto &[key, value] : batHeaders) { headers[key] = value; }
		}

		auto r = HttpClient::postWithHeaders(url, headers, "");

		if (r.status_code < 200 || r.status_code >= 300) {
			LOG_ERROR("Failed to fetch auth ticket: HTTP " + std::to_string(r.status_code));
			return "";
		}

		auto ticket = r.headers.find("rbx-authentication-ticket");
		if (ticket == r.headers.end()) {
			LOG_ERROR("Failed to get authentication ticket from response");
			return "";
		}

		return ticket->second;
	}

	// ============================================================================
	// Legacy methods (without HBA - for backward compatibility)
	// ============================================================================

	/**
	 * Get authenticated user info (legacy, no HBA)
	 * @param cookie The .ROBLOSECURITY cookie
	 * @return JSON object with user info, or empty object on failure
	 */
	static nlohmann::json getAuthenticatedUser(const std::string &cookie) {
		// Create config without HBA for backward compatibility
		HBA::AuthConfig config {.cookie = cookie, .hbaPrivateKey = "", .hbaEnabled = false};
		return getAuthenticatedUser(config);
	}

	/**
	 * Fetch authentication ticket (legacy, no HBA)
	 * @param cookie The .ROBLOSECURITY cookie
	 * @return Authentication ticket string, or empty on failure
	 */
	static std::string fetchAuthTicket(const std::string &cookie) {
		// Create config without HBA for backward compatibility
		HBA::AuthConfig config {.cookie = cookie, .hbaPrivateKey = "", .hbaEnabled = false};
		return fetchAuthTicket(config);
	}

	// ============================================================================
	// Utility methods
	// ============================================================================

	static uint64_t getUserId(const std::string &cookie) {
		auto userJson = getAuthenticatedUser(cookie);
		return userJson.value("id", 0ULL);
	}

	static uint64_t getUserId(const HBA::AuthConfig &config) {
		auto userJson = getAuthenticatedUser(config);
		return userJson.value("id", 0ULL);
	}

	static std::string getUsername(const std::string &cookie) {
		auto userJson = getAuthenticatedUser(cookie);
		return userJson.value("name", "");
	}

	static std::string getUsername(const HBA::AuthConfig &config) {
		auto userJson = getAuthenticatedUser(config);
		return userJson.value("name", "");
	}

	static std::string getDisplayName(const std::string &cookie) {
		auto userJson = getAuthenticatedUser(cookie);
		return userJson.value("displayName", "");
	}

	static std::string getDisplayName(const HBA::AuthConfig &config) {
		auto userJson = getAuthenticatedUser(config);
		return userJson.value("displayName", "");
	}

	/**
	 * Create an HBA AuthConfig from account credentials
	 * @param cookie The .ROBLOSECURITY cookie
	 * @param hbaPrivateKey PEM-encoded private key (empty to disable HBA)
	 * @param hbaEnabled Whether HBA is enabled for this account
	 * @return AuthConfig structure
	 */
	static HBA::AuthConfig
		makeAuthConfig(const std::string &cookie, const std::string &hbaPrivateKey = "", bool hbaEnabled = true) {
		return HBA::AuthConfig {
			.cookie = cookie,
			.hbaPrivateKey = hbaPrivateKey,
			.hbaEnabled = hbaEnabled && !hbaPrivateKey.empty()
		};
	}

} // namespace Roblox
