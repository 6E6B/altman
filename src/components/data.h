#ifndef DATA_H
#define DATA_H

#include <vector>
#include <string>
#include <set>
#include <array>
#include <ctime>
#include <unordered_map>
#include <imgui.h>
#include "history/log_types.h"

struct AccountData {
	int id = 0;
	std::string displayName;
	std::string username;
	std::string userId;
	std::string status;
	std::string voiceStatus;
	time_t voiceBanExpiry = 0;
	time_t banExpiry = 0;
	std::string note;
	std::string cookie;
	bool isFavorite = false;
	// For InGame status tooltip
	std::string lastLocation;
};

struct FavoriteGame {
	std::string name;
	uint64_t universeId;
	uint64_t placeId;
};

struct FriendInfo {
	uint64_t id = 0;
	std::string username;
	std::string displayName;

	std::string presence;
	std::string lastLocation;
	uint64_t placeId = 0;
	std::string gameId;
};

extern std::vector<FavoriteGame> g_favorites;
extern std::vector<AccountData> g_accounts;
extern std::vector<FriendInfo> g_friends;
extern std::unordered_map<int, std::vector<FriendInfo> > g_accountFriends;
extern std::unordered_map<int, std::vector<FriendInfo> > g_unfriendedFriends;
extern std::set<int> g_selectedAccountIds;
extern ImVec4 g_accentColor;

extern int g_defaultAccountId;
extern int g_statusRefreshInterval;
extern bool g_checkUpdatesOnStartup;
extern bool g_killRobloxOnLaunch;
extern bool g_clearCacheOnLaunch;
extern std::array<char, 128> s_jobIdBuffer;
extern std::array<char, 128> s_playerBuffer;

namespace Data {
	void LoadSettings(const std::string &filename = "settings.json");

	void SaveSettings(const std::string &filename = "settings.json");

	void SaveAccounts(const std::string &filename = "accounts.json");

	void LoadAccounts(const std::string &filename = "accounts.json");

	void LoadFavorites(const std::string &filename = "favorites.json");

	void SaveFavorites(const std::string &filename = "favorites.json");

	void LoadFriends(const std::string &filename = "friends.json");

	void SaveFriends(const std::string &filename = "friends.json");

	std::vector<LogInfo> LoadLogHistory(const std::string &filename = "log_history.json");

	void SaveLogHistory(const std::vector<LogInfo> &logs, const std::string &filename = "log_history.json");

	std::string StorageFilePath(const std::string &filename);
}

#endif
