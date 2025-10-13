#include "network/http.hpp"
#include <cctype>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>
#include <vector>
#include <windows.h>

#include "../../components/data.h"
#include "core/logging.hpp"
#include "roblox_control.h"
#include "ui/notifications.h"

static std::string urlEncode(const std::string &s) {
	std::ostringstream out;
	out << std::hex << std::uppercase;
	for (unsigned char c : s) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out << c;
		} else {
			out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
		}
	}
	return out.str();
}

inline HANDLE startRoblox(uint64_t placeId, const std::string &jobId, const std::string &cookie) {
	// Status::Set("Fetching x-csrf token"); // REMOVE THIS
	LOG_INFO("Fetching x-csrf token");
	auto csrfResponse = HttpClient::post(
		"https://auth.roblox.com/v1/authentication-ticket",
		{
			{"Cookie", ".ROBLOSECURITY=" + cookie}
	}
	);

	auto csrfToken = csrfResponse.headers.find("x-csrf-token");
	if (csrfToken == csrfResponse.headers.end()) {
		std::cerr << "failed to get CSRF token\n";

		// Status::Set("Failed to get CSRF token"); // REMOVE THIS
		LOG_ERROR("Failed to get CSRF token"); // This will now set the status
		return nullptr;
	}

	// Status::Set("Fetching authentication ticket"); // REMOVE THIS
	LOG_INFO("Fetching authentication ticket"); // This will now set the status
	auto ticketResponse = HttpClient::post(
		"https://auth.roblox.com/v1/authentication-ticket",
		{
			{"Cookie",	   ".ROBLOSECURITY=" + cookie},
			{"Origin",	   "https://www.roblox.com"  },
			{"Referer",		"https://www.roblox.com/" },
			{"X-CSRF-TOKEN", csrfToken->second		  }
	}
	);

	auto ticket = ticketResponse.headers.find("rbx-authentication-ticket");
	if (ticket == ticketResponse.headers.end()) {
		std::cerr << "failed to get authentication ticket\n";
		// Status::Set("Failed to get authentication ticket"); // REMOVE THIS
		LOG_ERROR("Failed to get authentication ticket"); // This will now set the status
		return nullptr;
	}

	auto nowMs
		= std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
			  .count();
	std::ostringstream ts;
	ts << nowMs;

	std::string placeLauncherUrl = "https://assetgame.roblox.com/game/PlaceLauncher.ashx?"
								   "request=RequestGameJob"
								   "&browserTrackerId=147062882894"
								   "&placeId="
								 + std::to_string(placeId) + "&gameId=" + jobId
								 + "&isPlayTogetherGame=false"
								   "+browsertrackerid:147062882894"
								   "+robloxLocale:en_us"
								   "+gameLocale:en_us"
								   "+channel:";

	std::string protocolLaunchCommand = "roblox-player:1+launchmode:play"
										"+gameinfo:"
									  + ticket->second + "+launchtime:" + ts.str()
									  + "+placelauncherurl:" + urlEncode(placeLauncherUrl);

	std::string logMessage = "Attempting to launch Roblox for place ID: " + std::to_string(placeId)
						   + (jobId.empty() ? "" : " with Job ID: " + jobId);
	LOG_INFO(logMessage);

	std::wstring notificationTitle = L"Launching";
	std::wostringstream notificationMessageStream;
	notificationMessageStream << L"Attempting to launch Roblox for place ID: " << placeId;
	if (!jobId.empty()) { notificationMessageStream << L" with Job ID: " << jobId.c_str(); }
	Notifications::showNotification(notificationTitle.c_str(), notificationMessageStream.str().c_str());

	SHELLEXECUTEINFOA executionInfo {sizeof(executionInfo)};
	executionInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
	executionInfo.lpVerb = "open";
	executionInfo.lpFile = protocolLaunchCommand.c_str();
	executionInfo.nShow = SW_SHOWNORMAL;

	if (!ShellExecuteExA(&executionInfo)) {
		LOG_ERROR("ShellExecuteExA failed for Roblox launch. Error: " + std::to_string(GetLastError()));
		std::cerr << "ShellExecuteEx failed: " << GetLastError() << "\n";
		return nullptr;
	}

	LOG_INFO("Roblox process started successfully for place ID: " + std::to_string(placeId));
	return executionInfo.hProcess;
}

inline void launchRobloxSequential(
	uint64_t placeId,
	const std::string &jobId,
	const std::vector<std::pair<int, std::string>> &accounts
) {
	if (g_killRobloxOnLaunch) { RobloxControl::KillRobloxProcesses(); }

	if (g_clearCacheOnLaunch) { RobloxControl::ClearRobloxCache(); }

	for (const auto &[accountId, cookie] : accounts) {
		LOG_INFO(
			"Launching Roblox for account ID: " + std::to_string(accountId) + " PlaceID: " + std::to_string(placeId)
			+ (jobId.empty() ? "" : " JobID: " + jobId)
		);
		HANDLE proc = startRoblox(placeId, jobId, cookie);
		if (proc) {
			WaitForInputIdle(proc, INFINITE);
			CloseHandle(proc);
			LOG_INFO("Roblox launched successfully for account ID: " + std::to_string(accountId));
		} else {
			LOG_ERROR("Failed to start Roblox for account ID: " + std::to_string(accountId));
		}
	}
}
