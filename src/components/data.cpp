#include "data.h"

#ifdef _WIN32
    #include <windows.h>
    #include <dpapi.h>
#elif __APPLE__
    #include <Security/Security.h>
    #include <CommonCrypto/CommonCrypto.h>
#endif

#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <memory>
#include <unordered_set>
#include <ranges>
#include <algorithm>
#include <format>
#include <optional>
#include <span>

#include "utils/account_utils.h"
#include "utils/base64.h"
#include "utils/paths.h"
#include "console/console.h"

namespace {

#ifdef _WIN32
    std::vector<std::uint8_t> encryptData(std::string_view plainText) {
        DATA_BLOB dataIn{
            .cbData = static_cast<DWORD>(plainText.size() + 1),
            .pbData = const_cast<std::uint8_t*>(
                reinterpret_cast<const std::uint8_t*>(plainText.data()))
        };
        DATA_BLOB dataOut{};
        constexpr auto description = L"User Cookie Data";

        if (!CryptProtectData(&dataIn, description, nullptr, nullptr, nullptr,
                             CRYPTPROTECT_UI_FORBIDDEN, &dataOut)) {
            LOG_ERROR("CryptProtectData failed. Error code: {}", GetLastError());
            throw std::runtime_error("Encryption failed");
        }

        std::vector<std::uint8_t> encrypted(dataOut.pbData, dataOut.pbData + dataOut.cbData);
        LocalFree(dataOut.pbData);
        return encrypted;
    }

    std::string decryptData(std::span<const std::uint8_t> encryptedData) {
        DATA_BLOB dataIn{
            .cbData = static_cast<DWORD>(encryptedData.size()),
            .pbData = const_cast<std::uint8_t*>(encryptedData.data())
        };
        DATA_BLOB dataOut{};
        LPWSTR descriptionOut = nullptr;

        if (!CryptUnprotectData(&dataIn, &descriptionOut, nullptr, nullptr, nullptr,
                               CRYPTPROTECT_UI_FORBIDDEN, &dataOut)) {
            const DWORD error = GetLastError();
            LOG_ERROR("CryptUnprotectData failed. Error code: {}", error);

            if (error == ERROR_INVALID_DATA || error == 0x8009000B) {
                LOG_ERROR("Could not decrypt data. It might be from a different user/machine or corrupted.");
            }
            return "";
        }

        std::string decrypted(reinterpret_cast<char*>(dataOut.pbData), dataOut.cbData);
        LocalFree(dataOut.pbData);
        if (descriptionOut) LocalFree(descriptionOut);

        if (!decrypted.empty() && decrypted.back() == '\0') {
            decrypted.pop_back();
        }
        return decrypted;
    }

#elif __APPLE__
	std::vector<std::uint8_t> encryptData(std::string_view plainText) {
    	return std::vector<std::uint8_t>(plainText.begin(), plainText.end());
    }

	std::string decryptData(std::span<const std::uint8_t> encryptedData) {
    	return std::string(encryptedData.begin(), encryptedData.end());
    }
#endif

    std::optional<std::string> encryptCookie(std::string_view cookie) {
        if (cookie.empty())
            return "";

        try {
            const auto encrypted = encryptData(cookie);
            return base64_encode(encrypted);
        } catch (const std::exception& e) {
            LOG_ERROR("Cookie encryption failed: {}", e.what());
            return std::nullopt;
        }
    }

    std::string decryptCookie(std::string_view base64Encrypted) {
        if (base64Encrypted.empty())
            return "";

        try {
            const auto encrypted = base64_decode(base64Encrypted);
            return decryptData(encrypted);
        } catch (const std::exception& e) {
            LOG_ERROR("Cookie decryption failed: {}", e.what());
            return "";
        }
    }

    template<typename T>
    T safeGet(const nlohmann::json& j, std::string_view key, T defaultValue) {
        try {
            return j.value(key.data(), defaultValue);
        } catch (...) {
            return defaultValue;
        }
    }

    AccountData parseAccount(const nlohmann::json& item) {
        AccountData account{};
        account.id = safeGet(item, "id", 0);
        account.displayName = safeGet<std::string>(item, "displayName", "");
        account.username = safeGet<std::string>(item, "username", "");
        account.userId = safeGet<std::string>(item, "userId", "");
        account.status = safeGet<std::string>(item, "status", "");
        account.voiceStatus = safeGet<std::string>(item, "voiceStatus", "");
        account.voiceBanExpiry = safeGet(item, "voiceBanExpiry", 0);
        account.banExpiry = safeGet(item, "banExpiry", 0);
        account.note = safeGet<std::string>(item, "note", "");
        account.isFavorite = safeGet(item, "isFavorite", false);
        account.lastLocation = safeGet<std::string>(item, "lastLocation", "");
        account.placeId = safeGet(item, "placeId", 0ULL);
        account.jobId = safeGet<std::string>(item, "jobId", "");
        account.isUsingCustomClient = safeGet<bool>(item, "isUsingCustomClient", false);
        account.clientName = safeGet<std::string>(item, "clientName", "");
        account.customClientBase = safeGet<std::string>(item, "customClientBase", "");

        if (item.contains("encryptedCookie")) {
            const auto encrypted = safeGet<std::string>(item, "encryptedCookie", "");
            account.cookie = decryptCookie(encrypted);

            if (account.cookie.empty() && !encrypted.empty()) {
                LOG_ERROR("Failed to decrypt cookie for account ID {}", account.id);
            }
        } else if (item.contains("cookie")) {
            account.cookie = safeGet<std::string>(item, "cookie", "");
            LOG_INFO("Account ID {} has an unencrypted cookie. It will be encrypted on next save.", account.id);
        }

        return account;
    }

    nlohmann::json serializeAccount(const AccountData& account) {
        const auto encryptedCookie = encryptCookie(account.cookie).value_or("");

        return {
            {"id", account.id},
            {"displayName", account.displayName},
            {"username", account.username},
            {"userId", account.userId},
            {"status", account.status},
            {"voiceStatus", account.voiceStatus},
            {"voiceBanExpiry", account.voiceBanExpiry},
            {"banExpiry", account.banExpiry},
            {"note", account.note},
            {"encryptedCookie", encryptedCookie},
            {"isFavorite", account.isFavorite},
            {"lastLocation", account.lastLocation},
            {"placeId", account.placeId},
            {"jobId", account.jobId},
            {"isUsingCustomClient", account.isUsingCustomClient},
            {"clientName", account.clientName},
            {"customClientBase", account.customClientBase}
        };
    }

    std::vector<FriendInfo> parseFriendList(const nlohmann::json& arr) {
        std::vector<FriendInfo> result;
        result.reserve(arr.size());

        for (const auto& item : arr) {
            if (!item.is_object()) continue;

            FriendInfo info{};
            info.id = safeGet(item, "userId", 0ULL);
            info.username = safeGet<std::string>(item, "username", "");
            info.displayName = safeGet<std::string>(item, "displayName", "");
            result.push_back(std::move(info));
        }
        return result;
    }

    nlohmann::json serializeFriendList(const std::vector<FriendInfo>& friends) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& f : friends) {
            arr.push_back({
                {"userId", f.id},
                {"username", f.username},
                {"displayName", f.displayName}
            });
        }
        return arr;
    }

    std::unordered_map<std::string, int> buildUserIdToAccountIdMap() {
        std::unordered_map<std::string, int> mapping;
        mapping.reserve(g_accounts.size());

        for (const auto& acc : g_accounts) {
            if (!acc.userId.empty()) {
                mapping[acc.userId] = acc.id;
            }
        }
        return mapping;
    }

    std::unordered_map<int, std::string> buildAccountIdToUserIdMap() {
        std::unordered_map<int, std::string> mapping;
        mapping.reserve(g_accounts.size());

        for (const auto& acc : g_accounts) {
            if (!acc.userId.empty()) {
                mapping[acc.id] = acc.userId;
            }
        }
        return mapping;
    }

} // anonymous namespace

std::unordered_map<int, std::size_t> s_accountIndexCache;
bool s_accountIndexDirty = true;

void ensureAccountIndexValid() {
    if (!s_accountIndexDirty) return;

    s_accountIndexCache.clear();
    s_accountIndexCache.reserve(g_accounts.size());

    for (std::size_t i = 0; i < g_accounts.size(); ++i) {
        s_accountIndexCache[g_accounts[i].id] = i;
    }
    s_accountIndexDirty = false;
}

void invalidateAccountIndex() {
    s_accountIndexDirty = true;
}

AccountData* getAccountById(int id) {
    ensureAccountIndexValid();

    auto it = s_accountIndexCache.find(id);
    if (it == s_accountIndexCache.end() || it->second >= g_accounts.size()) {
        return nullptr;
    }
    return &g_accounts[it->second];
}

std::vector<AccountData*> getUsableSelectedAccounts() {
    std::vector<AccountData*> result;
    result.reserve(g_selectedAccountIds.size());

    for (int id : g_selectedAccountIds) {
        if (AccountData* acc = getAccountById(id)) {
            if (AccountFilters::IsAccountUsable(*acc)) {
                result.push_back(acc);
            }
        }
    }
    return result;
}

std::vector<const AccountData*> getSelectedAccountsOrdered() {
    std::vector<const AccountData*> result;
    result.reserve(g_selectedAccountIds.size());
    for (const auto& acc : g_accounts) {
        if (g_selectedAccountIds.contains(acc.id)) {
            result.push_back(&acc);
        }
    }
    return result;
}

std::vector<AccountData*> getSelectedAccountsOrderedMutable() {
    std::vector<AccountData*> result;
    result.reserve(g_selectedAccountIds.size());
    for (auto& acc : g_accounts) {
        if (g_selectedAccountIds.contains(acc.id)) {
            result.push_back(&acc);
        }
    }
    return result;
}

int getAccountIndexById(int id) {
    ensureAccountIndexValid();
    auto it = s_accountIndexCache.find(id);
    if (it == s_accountIndexCache.end() || it->second >= g_accounts.size()) {
        return -1;
    }
    return static_cast<int>(it->second);
}

std::string getPrimaryAccountCookie() {
    if (g_selectedAccountIds.empty()) return {};

    if (const AccountData* acc = getAccountById(*g_selectedAccountIds.begin())) {
        return acc->cookie;
    }
    return {};
}

namespace Data {

    void LoadAccounts(std::string_view filename) {
        const auto path = AltMan::Paths::Config(filename).string();
        std::ifstream fin{path};

        if (!fin.is_open()) {
            LOG_INFO("No {}, starting fresh", path);
            return;
        }

        try {
            nlohmann::json dataArray;
            fin >> dataArray;

            g_accounts.clear();
            g_accounts.reserve(dataArray.size());

            for (const auto& item : dataArray) {
                g_accounts.push_back(parseAccount(item));
            }

            invalidateAccountIndex();

            LOG_INFO("Loaded {} accounts", g_accounts.size());
        } catch (const nlohmann::json::parse_error& e) {
            LOG_ERROR("Failed to parse {}: {}", path, e.what());
        }
    }

    void SaveAccounts(std::string_view filename) {
        const auto path = AltMan::Paths::Config(filename).string();
        std::ofstream out{path};

        if (!out.is_open()) {
            LOG_ERROR("Could not open '{}' for writing", path);
            return;
        }

        nlohmann::json dataArray = nlohmann::json::array();
        for (const auto& account : g_accounts) {
            dataArray.push_back(serializeAccount(account));
        }

        out << dataArray.dump(4);
        LOG_INFO("Saved {} accounts", g_accounts.size());
    }

    void LoadFavorites(std::string_view filename) {
        const auto path = AltMan::Paths::Config(filename).string();
        std::ifstream fin{path};

        if (!fin.is_open()) {
            LOG_INFO("No {}, starting with 0 favourites", path);
            return;
        }

        try {
            nlohmann::json arr;
            fin >> arr;

            g_favorites.clear();
            g_favorites.reserve(arr.size());

            for (const auto& j : arr) {
                g_favorites.push_back(FavoriteGame{
                    .name = safeGet<std::string>(j, "name", ""),
                    .universeId = safeGet(j, "universeId", 0ULL),
                    .placeId = safeGet(j, "placeId", safeGet(j, "universeId", 0ULL))
                });
            }

            LOG_INFO("Loaded {} favourites", g_favorites.size());
        } catch (const std::exception& e) {
            LOG_INFO("Could not parse {}: {}", filename, e.what());
        }
    }

    void SaveFavorites(std::string_view filename) {
        const auto path = AltMan::Paths::Config(filename).string();
        std::ofstream out{path};

        if (!out.is_open()) {
            LOG_ERROR("Could not open '{}' for writing", path);
            return;
        }

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& fav : g_favorites) {
            arr.push_back({
                {"universeId", fav.universeId},
                {"placeId", fav.placeId},
                {"name", fav.name}
            });
        }

        out << arr.dump(4);
        LOG_INFO("Saved {} favourites", g_favorites.size());
    }

    void LoadSettings(std::string_view filename) {
        const auto path = AltMan::Paths::Config(filename).string();
        std::ifstream fin{path};

        if (!fin.is_open()) {
            LOG_INFO("No {}, using default settings", path);
            return;
        }

        try {
            nlohmann::json j;
            fin >> j;

            g_defaultAccountId = safeGet(j, "defaultAccountId", -1);
            g_statusRefreshInterval = safeGet(j, "statusRefreshInterval", 3);
            g_checkUpdatesOnStartup = safeGet(j, "checkUpdatesOnStartup", true);
            g_killRobloxOnLaunch = safeGet(j, "killRobloxOnLaunch", false);
            g_clearCacheOnLaunch = safeGet(j, "clearCacheOnLaunch", false);
            g_multiRobloxEnabled = safeGet(j, "multiRobloxEnabled", false);

            if (j.contains("clientKeys") && j["clientKeys"].is_object()) {
                g_clientKeys.clear();
                for (auto& [key, value] : j["clientKeys"].items()) {
                    if (value.is_string()) {
                        g_clientKeys[key] = value.get<std::string>();
                    }
                }
                LOG_INFO("Loaded {} client keys", g_clientKeys.size());
            }

            LOG_INFO("Default account ID = {}", g_defaultAccountId);
            LOG_INFO("Status refresh interval = {}", g_statusRefreshInterval);
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to parse {}: {}", filename, e.what());
        }
    }

    void SaveSettings(std::string_view filename) {
        const nlohmann::json j = {
            {"defaultAccountId", g_defaultAccountId},
            {"statusRefreshInterval", g_statusRefreshInterval},
            {"checkUpdatesOnStartup", g_checkUpdatesOnStartup},
            {"killRobloxOnLaunch", g_killRobloxOnLaunch},
            {"clearCacheOnLaunch", g_clearCacheOnLaunch},
            {"multiRobloxEnabled", g_multiRobloxEnabled},
            {"clientKeys", g_clientKeys}
        };

        const auto path = AltMan::Paths::Config(filename).string();
        std::ofstream out{path};

        if (!out.is_open()) {
            LOG_ERROR("Could not open {} for writing", path);
            return;
        }

        out << j.dump(4);
        LOG_INFO("Saved settings");
    }

    void LoadFriends(std::string_view filename) {
        const auto path = AltMan::Paths::Config(filename).string();
        std::ifstream fin{path};

        if (!fin.is_open()) {
            LOG_INFO("No {}, starting with empty friend lists", path);
            return;
        }

        try {
            nlohmann::json j;
            fin >> j;

            if (!j.is_object()) {
                LOG_ERROR("Invalid friends.json format");
                return;
            }

            g_accountFriends.clear();
            g_unfriendedFriends.clear();

            const auto userIdToAccountId = buildUserIdToAccountIdMap();

            for (auto it = j.begin(); it != j.end(); ++it) {
                const std::string keyUserId = it.key();

                if (const auto mapIt = userIdToAccountId.find(keyUserId);
                    mapIt != userIdToAccountId.end() && it.value().is_object()) {

                    const int accountId = mapIt->second;
                    const auto& accountObj = it.value();

                    std::vector<FriendInfo> friends;
                    if (accountObj.contains("friends") && accountObj["friends"].is_array()) {
                        friends = parseFriendList(accountObj["friends"]);
                    }

                    std::vector<FriendInfo> unfriended;
                    if (accountObj.contains("unfriended") && accountObj["unfriended"].is_array()) {
                        unfriended = parseFriendList(accountObj["unfriended"]);
                    }

                    const auto friendIds = friends
                        | std::views::transform(&FriendInfo::id)
                        | std::ranges::to<std::unordered_set>();

                    std::unordered_set<uint64_t> seenUnfriended;
                    std::vector<FriendInfo> filteredUnfriended;

                    for (auto& u : unfriended) {
                        if (!friendIds.contains(u.id) && seenUnfriended.insert(u.id).second) {
                            filteredUnfriended.push_back(std::move(u));
                        }
                    }

                    g_accountFriends[accountId] = std::move(friends);
                    g_unfriendedFriends[accountId] = std::move(filteredUnfriended);
                }
            }

            LOG_INFO("Loaded friend data for {} accounts", g_accountFriends.size());
        } catch (const std::exception& e) {
            LOG_INFO("Failed to parse {}: {}", filename, e.what());
        }
    }

    void SaveFriends(std::string_view filename) {
        const auto path = AltMan::Paths::Config(filename).string();
        nlohmann::json root = nlohmann::json::object();

        const auto accountIdToUserId = buildAccountIdToUserIdMap();

        for (const auto& [accountId, friends] : g_accountFriends) {
            if (const auto it = accountIdToUserId.find(accountId); it != accountIdToUserId.end()) {
                const auto& userId = it->second;
                if (!root.contains(userId)) {
                    root[userId] = nlohmann::json::object();
                }
                root[userId]["friends"] = serializeFriendList(friends);
            }
        }

        for (const auto& [accountId, unfriended] : g_unfriendedFriends) {
            if (const auto& it = accountIdToUserId.find(accountId); it != accountIdToUserId.end()) {
                const auto& userId = it->second;
                if (!root.contains(userId)) {
                    root[userId] = nlohmann::json::object();
                }
                root[userId]["unfriended"] = serializeFriendList(unfriended);
            }
        }

        std::ofstream out{path};
        if (!out.is_open()) {
            LOG_ERROR("Could not open '{}' for writing", path);
            return;
        }

        out << root.dump(4);
        LOG_INFO("Saved friend data");
    }

}