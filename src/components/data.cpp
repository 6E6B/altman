#include "data.h"
#include "utils/crypto.h"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <unordered_set>

#include "console/console.h"
#include "utils/account_utils.h"
#include "utils/base64.h"
#include "utils/paths.h"

namespace {

    std::optional<std::array<std::uint8_t, crypto_secretbox_KEYBYTES>> s_localKey;

    std::expected<std::array<std::uint8_t, crypto_secretbox_KEYBYTES>, Crypto::Error> getOrCreateLocalKey() {
        if (s_localKey) {
            return *s_localKey;
        }

        const auto keyPath = AltMan::Paths::Config(".cookie_key");

        if (std::filesystem::exists(keyPath)) {
            std::ifstream keyFile(keyPath, std::ios::binary);
            if (keyFile.is_open()) {
                std::array<std::uint8_t, crypto_secretbox_KEYBYTES> key {};
                if (keyFile.read(reinterpret_cast<char *>(key.data()), static_cast<std::streamsize>(key.size()))) {
                    s_localKey = key;
                    LOG_INFO("Loaded local encryption key");
                    return key;
                }
            }
        }

        LOG_INFO("Generating new local encryption key");
        std::array<std::uint8_t, crypto_secretbox_KEYBYTES> key {};
        randombytes_buf(key.data(), key.size());

        std::error_code ec;
        std::filesystem::create_directories(keyPath.parent_path(), ec);

        std::ofstream outFile(keyPath, std::ios::binary);
        if (!outFile.is_open()) {
            LOG_ERROR("Failed to save local encryption key");
            return std::unexpected(Crypto::Error::EncryptionFailed);
        }

        outFile.write(reinterpret_cast<const char *>(key.data()), static_cast<std::streamsize>(key.size()));
        outFile.close();

#ifndef _WIN32
        std::filesystem::permissions(
            keyPath,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace,
            ec
        );
#endif

        s_localKey = key;
        return key;
    }

    std::optional<std::string> encryptCookie(std::string_view cookie) {
        if (cookie.empty()) {
            return "";
        }

        auto keyResult = getOrCreateLocalKey();
        if (!keyResult) {
            LOG_ERROR("Failed to get encryption key: {}", Crypto::errorToString(keyResult.error()));
            return std::nullopt;
        }
        const auto &key = *keyResult;

        std::array<std::uint8_t, crypto_secretbox_NONCEBYTES> nonce {};
        randombytes_buf(nonce.data(), nonce.size());

        std::vector<std::uint8_t> ciphertext(cookie.size() + crypto_secretbox_MACBYTES);

        if (crypto_secretbox_easy(
                ciphertext.data(),
                reinterpret_cast<const std::uint8_t *>(cookie.data()),
                cookie.size(),
                nonce.data(),
                key.data()
            )
            != 0) {
            LOG_ERROR("Cookie encryption failed");
            return std::nullopt;
        }

        std::vector<std::uint8_t> result;
        result.reserve(nonce.size() + ciphertext.size());
        result.insert(result.end(), nonce.begin(), nonce.end());
        result.insert(result.end(), ciphertext.begin(), ciphertext.end());

        return base64_encode(result);
    }

    std::string decryptCookie(std::string_view base64Encrypted) {
        if (base64Encrypted.empty()) {
            return "";
        }

        auto keyResult = getOrCreateLocalKey();
        if (!keyResult) {
            LOG_ERROR("Failed to get encryption key: {}", Crypto::errorToString(keyResult.error()));
            return "";
        }
        const auto &key = *keyResult;

        std::vector<std::uint8_t> encrypted;
        try {
            encrypted = base64_decode(base64Encrypted);
        } catch (const std::exception &e) {
            LOG_ERROR("Failed to decode base64 cookie: {}", e.what());
            return "";
        }

        constexpr std::size_t kMinSize = crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES;
        if (encrypted.size() < kMinSize) {
            LOG_ERROR("Encrypted cookie too short ({} bytes, need at least {})", encrypted.size(), kMinSize);
            return "";
        }

        const std::uint8_t *nonce = encrypted.data();
        const std::uint8_t *ciphertext = encrypted.data() + crypto_secretbox_NONCEBYTES;
        const std::size_t ciphertextLen = encrypted.size() - crypto_secretbox_NONCEBYTES;

        std::vector<std::uint8_t> plaintext(ciphertextLen - crypto_secretbox_MACBYTES);

        if (crypto_secretbox_open_easy(plaintext.data(), ciphertext, ciphertextLen, nonce, key.data()) != 0) {
            LOG_ERROR("Cookie decryption failed (wrong key or corrupted data)");
            return "";
        }

        return std::string(plaintext.begin(), plaintext.end());
    }

    template<typename T> T safeGet(const nlohmann::json &j, std::string_view key, T defaultValue) {
        try {
            return j.value(key.data(), defaultValue);
        } catch (...) {
            return defaultValue;
        }
    }

    AccountData parseAccount(const nlohmann::json &item) {
        AccountData account {};
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
        }

        return account;
    }

    nlohmann::json serializeAccount(const AccountData &account) {
        const auto encryptedCookie = encryptCookie(account.cookie).value_or("");

        return {
            {"id",                  account.id                 },
            {"displayName",         account.displayName        },
            {"username",            account.username           },
            {"userId",              account.userId             },
            {"status",              account.status             },
            {"voiceStatus",         account.voiceStatus        },
            {"voiceBanExpiry",      account.voiceBanExpiry     },
            {"banExpiry",           account.banExpiry          },
            {"note",                account.note               },
            {"encryptedCookie",     encryptedCookie            },
            {"isFavorite",          account.isFavorite         },
            {"lastLocation",        account.lastLocation       },
            {"placeId",             account.placeId            },
            {"jobId",               account.jobId              },
            {"isUsingCustomClient", account.isUsingCustomClient},
            {"clientName",          account.clientName         },
            {"customClientBase",    account.customClientBase   }
        };
    }

    std::vector<FriendInfo> parseFriendList(const nlohmann::json &arr) {
        std::vector<FriendInfo> result;
        result.reserve(arr.size());

        for (const auto &item: arr) {
            if (!item.is_object()) {
                continue;
            }

            FriendInfo info {};
            info.id = safeGet(item, "userId", 0ULL);
            info.username = safeGet<std::string>(item, "username", "");
            info.displayName = safeGet<std::string>(item, "displayName", "");
            result.push_back(std::move(info));
        }
        return result;
    }

    nlohmann::json serializeFriendList(const std::vector<FriendInfo> &friends) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &f: friends) {
            arr.push_back({
                {"userId",      f.id         },
                {"username",    f.username   },
                {"displayName", f.displayName}
            });
        }
        return arr;
    }

    std::unordered_map<std::string, int> buildUserIdToAccountIdMap() {
        std::unordered_map<std::string, int> mapping;
        mapping.reserve(g_accounts.size());

        for (const auto &acc: g_accounts) {
            if (!acc.userId.empty()) {
                mapping[acc.userId] = acc.id;
            }
        }
        return mapping;
    }

    std::unordered_map<int, std::string> buildAccountIdToUserIdMap() {
        std::unordered_map<int, std::string> mapping;
        mapping.reserve(g_accounts.size());

        for (const auto &acc: g_accounts) {
            if (!acc.userId.empty()) {
                mapping[acc.id] = acc.userId;
            }
        }
        return mapping;
    }

} // namespace

std::unordered_map<int, std::size_t> s_accountIndexCache;
bool s_accountIndexDirty = true;

void ensureAccountIndexValid() {
    if (!s_accountIndexDirty) {
        return;
    }

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

AccountData *getAccountById(int id) {
    ensureAccountIndexValid();

    auto it = s_accountIndexCache.find(id);
    if (it == s_accountIndexCache.end() || it->second >= g_accounts.size()) {
        return nullptr;
    }
    return &g_accounts[it->second];
}

std::vector<AccountData *> getUsableSelectedAccounts() {
    std::vector<AccountData *> result;
    result.reserve(g_selectedAccountIds.size());

    for (int id: g_selectedAccountIds) {
        if (AccountData *acc = getAccountById(id)) {
            if (AccountFilters::IsAccountUsable(*acc)) {
                result.push_back(acc);
            }
        }
    }
    return result;
}

std::vector<const AccountData *> getSelectedAccountsOrdered() {
    std::vector<const AccountData *> result;
    result.reserve(g_selectedAccountIds.size());
    for (const auto &acc: g_accounts) {
        if (g_selectedAccountIds.contains(acc.id)) {
            result.push_back(&acc);
        }
    }
    return result;
}

std::vector<AccountData *> getSelectedAccountsOrderedMutable() {
    std::vector<AccountData *> result;
    result.reserve(g_selectedAccountIds.size());
    for (auto &acc: g_accounts) {
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
    if (g_selectedAccountIds.empty()) {
        return {};
    }

    if (const AccountData *acc = getAccountById(*g_selectedAccountIds.begin())) {
        return acc->cookie;
    }
    return {};
}

namespace Data {

    void LoadAccounts(std::string_view filename) {
        const auto path = AltMan::Paths::Config(filename).string();
        std::ifstream fin {path};

        if (!fin.is_open()) {
            LOG_INFO("No {}, starting fresh", path);
            return;
        }

        try {
            nlohmann::json dataArray;
            fin >> dataArray;

            g_accounts.clear();
            g_accounts.reserve(dataArray.size());

            for (const auto &item: dataArray) {
                g_accounts.push_back(parseAccount(item));
            }

            invalidateAccountIndex();

            LOG_INFO("Loaded {} accounts", g_accounts.size());
        } catch (const nlohmann::json::parse_error &e) {
            LOG_ERROR("Failed to parse {}: {}", path, e.what());
        }
    }

    void SaveAccounts(std::string_view filename) {
        const auto path = AltMan::Paths::Config(filename).string();
        std::ofstream out {path};

        if (!out.is_open()) {
            LOG_ERROR("Could not open '{}' for writing", path);
            return;
        }

        nlohmann::json dataArray = nlohmann::json::array();
        for (const auto &account: g_accounts) {
            dataArray.push_back(serializeAccount(account));
        }

        out << dataArray.dump(4);
        LOG_INFO("Saved {} accounts", g_accounts.size());
    }

    void LoadFavorites(std::string_view filename) {
        const auto path = AltMan::Paths::Config(filename).string();
        std::ifstream fin {path};

        if (!fin.is_open()) {
            LOG_INFO("No {}, starting with 0 favourites", path);
            return;
        }

        try {
            nlohmann::json arr;
            fin >> arr;

            g_favorites.clear();
            g_favorites.reserve(arr.size());

            for (const auto &j: arr) {
                g_favorites.push_back(
                    FavoriteGame {
                        .name = safeGet<std::string>(j, "name", ""),
                        .universeId = safeGet(j, "universeId", 0ULL),
                        .placeId = safeGet(j, "placeId", safeGet(j, "universeId", 0ULL))
                    }
                );
            }

            LOG_INFO("Loaded {} favourites", g_favorites.size());
        } catch (const std::exception &e) {
            LOG_INFO("Could not parse {}: {}", filename, e.what());
        }
    }

    void SaveFavorites(std::string_view filename) {
        const auto path = AltMan::Paths::Config(filename).string();
        std::ofstream out {path};

        if (!out.is_open()) {
            LOG_ERROR("Could not open '{}' for writing", path);
            return;
        }

        nlohmann::json arr = nlohmann::json::array();
        for (const auto &fav: g_favorites) {
            arr.push_back({
                {"universeId", fav.universeId},
                {"placeId",    fav.placeId   },
                {"name",       fav.name      }
            });
        }

        out << arr.dump(4);
        LOG_INFO("Saved {} favourites", g_favorites.size());
    }

    void LoadSettings(std::string_view filename) {
        const auto path = AltMan::Paths::Config(filename).string();
        std::ifstream fin {path};

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
                for (auto &[key, value]: j["clientKeys"].items()) {
                    if (value.is_string()) {
                        g_clientKeys[key] = value.get<std::string>();
                    }
                }
                LOG_INFO("Loaded {} client keys", g_clientKeys.size());
            }

            LOG_INFO("Default account ID = {}", g_defaultAccountId);
            LOG_INFO("Status refresh interval = {}", g_statusRefreshInterval);
        } catch (const std::exception &e) {
            LOG_ERROR("Failed to parse {}: {}", filename, e.what());
        }
    }

    void SaveSettings(std::string_view filename) {
        const nlohmann::json j = {
            {"defaultAccountId",      g_defaultAccountId     },
            {"statusRefreshInterval", g_statusRefreshInterval},
            {"checkUpdatesOnStartup", g_checkUpdatesOnStartup},
            {"killRobloxOnLaunch",    g_killRobloxOnLaunch   },
            {"clearCacheOnLaunch",    g_clearCacheOnLaunch   },
            {"multiRobloxEnabled",    g_multiRobloxEnabled   },
            {"clientKeys",            g_clientKeys           }
        };

        const auto path = AltMan::Paths::Config(filename).string();
        std::ofstream out {path};

        if (!out.is_open()) {
            LOG_ERROR("Could not open {} for writing", path);
            return;
        }

        out << j.dump(4);
        LOG_INFO("Saved settings");
    }

    void LoadFriends(std::string_view filename) {
        const auto path = AltMan::Paths::Config(filename).string();
        std::ifstream fin {path};

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
                    const auto &accountObj = it.value();

                    std::vector<FriendInfo> friends;
                    if (accountObj.contains("friends") && accountObj["friends"].is_array()) {
                        friends = parseFriendList(accountObj["friends"]);
                    }

                    std::vector<FriendInfo> unfriended;
                    if (accountObj.contains("unfriended") && accountObj["unfriended"].is_array()) {
                        unfriended = parseFriendList(accountObj["unfriended"]);
                    }

                    const auto friendIds
                        = friends | std::views::transform(&FriendInfo::id) | std::ranges::to<std::unordered_set>();

                    std::unordered_set<uint64_t> seenUnfriended;
                    std::vector<FriendInfo> filteredUnfriended;

                    for (auto &u: unfriended) {
                        if (!friendIds.contains(u.id) && seenUnfriended.insert(u.id).second) {
                            filteredUnfriended.push_back(std::move(u));
                        }
                    }

                    g_accountFriends[accountId] = std::move(friends);
                    g_unfriendedFriends[accountId] = std::move(filteredUnfriended);
                }
            }

            LOG_INFO("Loaded friend data for {} accounts", g_accountFriends.size());
        } catch (const std::exception &e) {
            LOG_INFO("Failed to parse {}: {}", filename, e.what());
        }
    }

    void SaveFriends(std::string_view filename) {
        const auto path = AltMan::Paths::Config(filename).string();
        nlohmann::json root = nlohmann::json::object();

        const auto accountIdToUserId = buildAccountIdToUserIdMap();

        for (const auto &[accountId, friends]: g_accountFriends) {
            if (const auto it = accountIdToUserId.find(accountId); it != accountIdToUserId.end()) {
                const auto &userId = it->second;
                if (!root.contains(userId)) {
                    root[userId] = nlohmann::json::object();
                }
                root[userId]["friends"] = serializeFriendList(friends);
            }
        }

        for (const auto &[accountId, unfriended]: g_unfriendedFriends) {
            if (const auto &it = accountIdToUserId.find(accountId); it != accountIdToUserId.end()) {
                const auto &userId = it->second;
                if (!root.contains(userId)) {
                    root[userId] = nlohmann::json::object();
                }
                root[userId]["unfriended"] = serializeFriendList(unfriended);
            }
        }

        std::ofstream out {path};
        if (!out.is_open()) {
            LOG_ERROR("Could not open '{}' for writing", path);
            return;
        }

        out << root.dump(4);
        LOG_INFO("Saved friend data");
    }

} // namespace Data
