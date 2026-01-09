#pragma once

#include <string>
#include <vector>
#include <optional>
#include <utility>
#include <cstdint>

struct AccountData;

enum class LaunchMode {
	Job,                 // Join any server
	GameJob,             // Join specific server by JobID
	PrivateServer,       // Join private server via share link
	PrivateServerDirect, // Join private server directly
	FollowUser           // Follow a user into their game
};

struct LaunchParams {
	LaunchMode mode = LaunchMode::Job;
	uint64_t placeId = 0;
	std::string value;  // Multi-purpose: jobId, shareLink, or userId depending on mode

	static LaunchParams standard(uint64_t placeId);
	static LaunchParams gameJob(uint64_t placeId, const std::string& jobId);
	static LaunchParams privateServer(const std::string& shareLink);
	static LaunchParams privateServerDirect(uint64_t placeId, const std::string& accessCode);
	static LaunchParams followUser(const std::string& userId);
};

#ifdef _WIN32
#include <windows.h>
HANDLE startRoblox(uint64_t placeId, const std::string& jobId, const std::string& cookie);
#endif

#ifdef __APPLE__
bool patchRobloxBinary(const std::string& appPath);
bool copyClientToUserEnvironment(const std::string& username, const std::string& clientName);
bool createSandboxedRoblox(AccountData& acc, const std::string& protocolURL);
#endif

bool startRoblox(const LaunchParams& params, AccountData& acc);
void launchRobloxSequential(const LaunchParams& params, const std::vector<std::pair<int, std::string>>& accounts);

std::string urlEncode(const std::string& s);
std::string generateBrowserTrackerId();
std::string getCurrentTimestampMs();
std::optional<std::string> fetchCsrfToken(const std::string& cookie);
std::optional<std::string> fetchAuthTicket(const std::string& cookie, const std::string& csrfToken);