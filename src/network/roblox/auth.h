#pragma once

#include <string>
#include <ctime>
#include <cstdint>
#include <nlohmann/json.hpp>

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
		uint64_t punishedUserId = 0;
	};

	BanInfo checkBanStatus(const std::string& cookie);
	BanCheckResult cachedBanStatus(const std::string& cookie);
	BanCheckResult refreshBanStatus(const std::string& cookie);

	bool isCookieValid(const std::string& cookie);
	bool canUseCookie(const std::string& cookie);

	nlohmann::json getAuthenticatedUser(const std::string& cookie);
	std::string fetchAuthTicket(const std::string& cookie);
	uint64_t getUserId(const std::string& cookie);
	std::string getUsername(const std::string& cookie);
	std::string getDisplayName(const std::string& cookie);

}