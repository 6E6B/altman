#pragma once

#include <vector>
#include <string>
#include <string_view>
#include <set>
#include <array>
#include <mutex>
#include <shared_mutex>
#include <ctime>
#include <unordered_map>
#include <cstdint>

struct FriendInfo;
struct AccountData;
struct FavoriteGame;

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
    std::string lastLocation;
    uint64_t placeId = 0;
    std::string jobId;
    bool isUsingCustomClient = false;
    std::string clientName;
    std::string customClientBase;
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

inline std::mutex g_selectionMutex;
inline std::shared_mutex g_accountsMutex;

inline std::vector<AccountData> g_accounts;
inline std::set<int> g_selectedAccountIds;

inline std::vector<FavoriteGame> g_favorites;
inline std::vector<FriendInfo> g_friends;
inline std::unordered_map<int, std::vector<FriendInfo>> g_accountFriends;
inline std::unordered_map<int, std::vector<FriendInfo>> g_unfriendedFriends;

inline int g_defaultAccountId = -1;
inline std::array<char, 128> s_jobIdBuffer = {};
inline std::array<char, 128> s_playerBuffer = {};
inline int g_statusRefreshInterval = 3;
inline bool g_checkUpdatesOnStartup = true;
inline bool g_killRobloxOnLaunch = false;
inline bool g_clearCacheOnLaunch = false;
inline bool g_multiRobloxEnabled = false;
inline std::unordered_map<std::string, std::string> g_clientKeys;
inline bool g_forceLatestRobloxVersion = false;

void invalidateAccountIndex();
AccountData* getAccountById(int id);
std::vector<AccountData*> getUsableSelectedAccounts();
std::vector<const AccountData*> getSelectedAccountsOrdered();
std::vector<AccountData*> getSelectedAccountsOrderedMutable();
int getAccountIndexById(int id);
std::string getPrimaryAccountCookie();

namespace Data {
    void LoadSettings(std::string_view filename = "settings.json");
    void SaveSettings(std::string_view filename = "settings.json");

    void LoadAccounts(std::string_view filename = "accounts.json");
    void SaveAccounts(std::string_view filename = "accounts.json");

    void LoadFavorites(std::string_view filename = "favorites.json");
    void SaveFavorites(std::string_view filename = "favorites.json");

    void LoadFriends(std::string_view filename = "friends.json");
    void SaveFriends(std::string_view filename = "friends.json");
}