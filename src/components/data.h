#ifndef DATA_H
#define DATA_H

#include <vector>
#include <string>
#include <set>
#include <array>
#include <ctime>
#include <unordered_map>
#include <fstream>
#include <nlohmann/json.hpp>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <ranges>
#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
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
	// Cached presence details for join menu
	uint64_t placeId = 0;
	std::string jobId;
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
	std::string jobId;
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

#ifdef __APPLE__
std::string GetApplicationDir();
std::string GetApplicationName();
std::string GetExecutablePath();
#endif

namespace Data {
	void LoadSettings(std::string_view filename = "settings.json");

	void SaveSettings(std::string_view filename = "settings.json");

	void SaveAccounts(std::string_view filename = "accounts.json");

	void LoadAccounts(std::string_view filename = "accounts.json");

	void LoadFavorites(std::string_view filename = "favorites.json");

	void SaveFavorites(std::string_view filename = "favorites.json");

	void LoadFriends(std::string_view filename = "friends.json");

	void SaveFriends(std::string_view filename = "friends.json");


	std::string StorageFilePath(std::string_view filename);
}

#endif
