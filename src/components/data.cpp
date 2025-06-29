#define WIN32_LEAN_AND_MEAN
#include "data.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <filesystem>
#include <unordered_map>
#include <windows.h>
#include <dpapi.h>

#include "core/base64.h"
#include "core/logging.hpp"

#pragma comment(lib, "Crypt32.lib")

using namespace std;
using json = nlohmann::json;

vector<AccountData> g_accounts;
set<int> g_selectedAccountIds;

vector<FavoriteGame> g_favorites;
vector<FriendInfo> g_friends;
unordered_map<int, vector<FriendInfo>> g_accountFriends;
unordered_map<int, vector<FriendInfo>> g_unfriendedFriends;

int g_defaultAccountId = -1;
array<char, 128> s_jobIdBuffer = {};
array<char, 128> s_playerBuffer = {};
int g_statusRefreshInterval = 1;
bool g_checkUpdatesOnStartup = true;
bool g_killRobloxOnLaunch = false;
bool g_clearCacheOnLaunch = false;

vector<BYTE> encryptData(const string &plainText) {
    DATA_BLOB DataIn;
    DATA_BLOB DataOut;

    auto szDescription = L"User Cookie Data";

    DataIn.pbData = (BYTE *) plainText.c_str();
    DataIn.cbData = plainText.length() + 1;

    if (CryptProtectData(&DataIn, szDescription, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &DataOut)) {
        vector<BYTE> encrypted(DataOut.pbData, DataOut.pbData + DataOut.cbData);
        LocalFree(DataOut.pbData);
        return encrypted;
    }
    LOG_ERROR("CryptProtectData failed. Error code: " + std::to_string(GetLastError()));
    throw runtime_error("Encryption failed");
}

string decryptData(const vector<BYTE> &encryptedText) {
    DATA_BLOB DataIn;
    DATA_BLOB DataOut;
    LPWSTR szDescriptionOut = nullptr;

    DataIn.pbData = (BYTE *) encryptedText.data();
    DataIn.cbData = encryptedText.size();

    if (CryptUnprotectData(&DataIn, &szDescriptionOut, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                           &DataOut)) {
        string decrypted((char *) DataOut.pbData, DataOut.cbData);
        LocalFree(DataOut.pbData);
        if (szDescriptionOut)
            LocalFree(szDescriptionOut);

        if (!decrypted.empty() && decrypted.back() == '\0') {
            decrypted.pop_back();
        }
        return decrypted;
    }
    LOG_ERROR("CryptUnprotectData failed. Error code: " + std::to_string(GetLastError()));

    if (GetLastError() == ERROR_INVALID_DATA || GetLastError() == 0x8009000B) {
        LOG_ERROR("Could not decrypt data. It might be from a different user/machine or corrupted.");
    }

    return "";
}

static std::filesystem::path GetStorageDir() {
    static std::filesystem::path dir;
    if (dir.empty()) {
        wchar_t exePath[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
            dir = std::filesystem::path(exePath).parent_path() / L"storage";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (ec) {
                LOG_ERROR("Failed to create storage directory: " + ec.message());
            }
        } else {
            dir = L"storage";
        }
    }
    return dir;
}

static std::string MakePath(const std::string &filename) {
    return (GetStorageDir() / filename).string();
}

namespace Data {
    void LoadAccounts(const string &filename) {
        string path = MakePath(filename);
        ifstream fileStream{path};
        if (!fileStream.is_open()) {
            LOG_INFO("No " + path + ", starting fresh");
            return;
        }
        json dataArray;
        try {
            fileStream >> dataArray;
        } catch (const json::parse_error &exception) {
            LOG_ERROR("Failed to parse " + path + ": " + exception.what());

            return;
        }

        g_accounts.clear();
        for (auto &item: dataArray) {
            AccountData account;
            account.id = item.value("id", 0);
            account.displayName = item.value("displayName", "");
            account.username = item.value("username", "");
            account.userId = item.value("userId", "");
            account.status = item.value("status", "");
            account.voiceStatus = item.value("voiceStatus", "");
            account.voiceBanExpiry = item.value("voiceBanExpiry", 0);
            account.note = item.value("note", "");
            account.isFavorite = item.value("isFavorite", false);

            if (item.contains("encryptedCookie")) {
                string b64EncryptedCookie = item.value("encryptedCookie", "");
                if (!b64EncryptedCookie.empty()) {
                    try {
                        vector<BYTE> encryptedCookieBytes = base64_decode(b64EncryptedCookie);
                        account.cookie = decryptData(encryptedCookieBytes);
                        if (account.cookie.empty() && !encryptedCookieBytes.empty()) {
                            LOG_ERROR(
                                "Failed to decrypt cookie for account ID " + std::to_string(account.id) +
                                ". Cookie will be empty. User might need to re-authenticate.");
                        }
                    } catch (const exception &e) {
                        LOG_ERROR(
                            "Exception during cookie decryption for account ID " + std::to_string(account.id) + ": " + e
                            .what());
                        account.cookie = "";
                    }
                } else {
                    account.cookie = "";
                }
            } else if (item.contains("cookie")) {
                account.cookie = item.value("cookie", "");
                LOG_INFO(
                    "Account ID " + std::to_string(account.id) +
                    " has an unencrypted cookie. It will be encrypted on next save.");
            }

            g_accounts.push_back(move(account));
        }
        LOG_INFO("Loaded " + std::to_string(g_accounts.size()) + " accounts");
    }

    void SaveAccounts(const string &filename) {
        string path = MakePath(filename);
        ofstream out{path};
        if (!out.is_open()) {
            LOG_ERROR("Could not open '" + path + "' for writing");
            return;
        }

        json dataArray = json::array();
        for (auto &account: g_accounts) {
            string b64EncryptedCookie;
            if (!account.cookie.empty()) {
                try {
                    vector<BYTE> encryptedCookieBytes = encryptData(account.cookie);
                    b64EncryptedCookie = base64_encode(encryptedCookieBytes);
                } catch (const exception &exception) {
                    LOG_ERROR(
                        "Exception during cookie encryption for account ID " + std::to_string(account.id) + ": " +
                        exception.what() + ". Cookie will not be saved.");
                    b64EncryptedCookie = "";
                }
            }

            dataArray.push_back({
                {"id", account.id},
                {"displayName", account.displayName},
                {"username", account.username},
                {"userId", account.userId},
                {"status", account.status},
                {"voiceStatus", account.voiceStatus},
                {"voiceBanExpiry", account.voiceBanExpiry},
                {"note", account.note},
                {"encryptedCookie", b64EncryptedCookie},
                {"isFavorite", account.isFavorite}
            });
        }
        out << dataArray.dump(4);
        LOG_INFO("Saved " + std::to_string(g_accounts.size()) + " accounts");
    }

    void LoadFavorites(const std::string &filename) {
        std::string path = MakePath(filename);
        std::ifstream fin{path};
        if (!fin.is_open()) {
            LOG_INFO("No " + path + ", starting with 0 favourites");
            return;
        }

        g_favorites.clear();
        try {
            json arr;
            fin >> arr;
            for (auto &j: arr) {
                g_favorites.push_back(FavoriteGame{
                    j.value("name", ""),
                    j.value("universeId", 0ULL),
                    j.value("placeId", j.value("universeId", 0ULL))
                });
            }
            LOG_INFO("Loaded " + std::to_string(g_favorites.size()) + " favourites");
        } catch (const std::exception &e) {
            LOG_ERROR("Could not parse " + filename + ": " + e.what());
        }
    }

    void SaveFavorites(const std::string &filename) {
        std::string path = MakePath(filename);
        std::ofstream out{path};
        if (!out.is_open()) {
            LOG_ERROR("Could not open '" + path + "' for writing");
            return;
        }

        json arr = json::array();
        for (auto &f: g_favorites) {
            arr.push_back({
                {"universeId", f.universeId},
                {"placeId", f.placeId},
                {"name", f.name}
            });
        }

        out << arr.dump(4);
        LOG_INFO("Saved " + std::to_string(g_favorites.size()) + " favourites");
    }

    void LoadSettings(const std::string &filename) {
        std::string path = MakePath(filename);
        std::ifstream fin{path};
        if (!fin.is_open()) {
            LOG_INFO("No " + path + ", using no default account");
            return;
        }

        try {
            nlohmann::json j;
            fin >> j;
            g_defaultAccountId = j.value("defaultAccountId", -1);
            g_statusRefreshInterval = j.value("statusRefreshInterval", 1);
            g_checkUpdatesOnStartup = j.value("checkUpdatesOnStartup", true);
            g_killRobloxOnLaunch = j.value("killRobloxOnLaunch", false);
            g_clearCacheOnLaunch = j.value("clearCacheOnLaunch", false);
            LOG_INFO("Default account ID = " + std::to_string(g_defaultAccountId));
            LOG_INFO("Status refresh interval = " + std::to_string(g_statusRefreshInterval));
            LOG_INFO("Check updates on startup = " + std::string(g_checkUpdatesOnStartup ? "true" : "false"));
            LOG_INFO("Kill Roblox on launch = " + std::string(g_killRobloxOnLaunch ? "true" : "false"));
            LOG_INFO("Clear cache on launch = " + std::string(g_clearCacheOnLaunch ? "true" : "false"));
        } catch (const std::exception &e) {
            LOG_ERROR("Failed to parse " + filename + ": " + e.what());
        }
    }

    void SaveSettings(const std::string &filename) {
        nlohmann::json j;
        j["defaultAccountId"] = g_defaultAccountId;
        j["statusRefreshInterval"] = g_statusRefreshInterval;
        j["checkUpdatesOnStartup"] = g_checkUpdatesOnStartup;
        j["killRobloxOnLaunch"] = g_killRobloxOnLaunch;
        j["clearCacheOnLaunch"] = g_clearCacheOnLaunch;
        std::string path = MakePath(filename);
        std::ofstream out{path};
        if (!out.is_open()) {
            LOG_ERROR("Could not open " + path + " for writing");
            return;
        }
        out << j.dump(4);
        LOG_INFO("Saved defaultAccountId=" + std::to_string(g_defaultAccountId));
        LOG_INFO("Saved statusRefreshInterval=" + std::to_string(g_statusRefreshInterval));
        LOG_INFO("Saved checkUpdatesOnStartup=" + std::string(g_checkUpdatesOnStartup ? "true" : "false"));
        LOG_INFO("Saved killRobloxOnLaunch=" + std::string(g_killRobloxOnLaunch ? "true" : "false"));
        LOG_INFO("Saved clearCacheOnLaunch=" + std::string(g_clearCacheOnLaunch ? "true" : "false"));
    }

    void LoadFriends(const std::string &filename) {
        std::string path = MakePath(filename);
        std::ifstream fin{path};
        if (!fin.is_open()) {
            LOG_INFO("No " + path + ", starting with empty friend lists");
            return;
        }
        try {
            json j; fin >> j;
            g_accountFriends.clear();
            g_unfriendedFriends.clear();

            auto parseList = [](const json &arr) {
                std::vector<FriendInfo> out;
                for (auto &f : arr) {
                    FriendInfo fi;
                    fi.id = f.value("id", 0ULL);
                    fi.username = f.value("username", "");
                    fi.displayName = f.value("displayName", "");
                    out.push_back(std::move(fi));
                }
                return out;
            };

            if (j.contains("friends") || j.contains("unfriended")) {
                for (auto it = j["friends"].begin(); it != j["friends"].end(); ++it) {
                    int acctId = std::stoi(it.key());
                    g_accountFriends[acctId] = parseList(it.value());
                }
                if (j.contains("unfriended")) {
                    for (auto it = j["unfriended"].begin(); it != j["unfriended"].end(); ++it) {
                        int acctId = std::stoi(it.key());
                        g_unfriendedFriends[acctId] = parseList(it.value());
                    }
                }
            } else {
                for (auto it = j.begin(); it != j.end(); ++it) {
                    int acctId = std::stoi(it.key());
                    g_accountFriends[acctId] = parseList(it.value());
                }
            }

            LOG_INFO("Loaded friend data for " + std::to_string(g_accountFriends.size()) + " accounts");
        } catch (const std::exception &e) {
            LOG_ERROR("Failed to parse " + filename + ": " + e.what());
        }
    }

    void SaveFriends(const std::string &filename) {
        std::string path = MakePath(filename);
        json jFriends = json::object();
        for (const auto &[acctId, friends] : g_accountFriends) {
            json arr = json::array();
            for (const auto &f : friends) {
                arr.push_back({{"id", f.id}, {"username", f.username}, {"displayName", f.displayName}});
            }
            jFriends[std::to_string(acctId)] = std::move(arr);
        }

        json jUnfriended = json::object();
        for (const auto &[acctId, list] : g_unfriendedFriends) {
            json arr = json::array();
            for (const auto &f : list) {
                arr.push_back({{"id", f.id}, {"username", f.username}, {"displayName", f.displayName}});
            }
            jUnfriended[std::to_string(acctId)] = std::move(arr);
        }

        json j;
        j["friends"] = std::move(jFriends);
        j["unfriended"] = std::move(jUnfriended);

        std::ofstream out{path};
        if (!out.is_open()) {
            LOG_ERROR("Could not open '" + path + "' for writing");
            return;
        }
        out << j.dump(4);
        LOG_INFO("Saved friend data for " + std::to_string(g_accountFriends.size()) + " accounts");
    }

    std::vector<LogInfo> LoadLogHistory(const std::string &filename) {
        std::string path = MakePath(filename);
        std::ifstream fin{path};
        std::vector<LogInfo> logs;
        if (!fin.is_open()) {
            LOG_INFO("No " + path + ", starting with empty log history");
            return logs;
        }

        try {
            json arr; fin >> arr;
            for (auto &j : arr) {
                LogInfo info;
                info.fileName = j.value("fileName", "");
                info.fullPath = j.value("fullPath", "");
                info.timestamp = j.value("timestamp", "");
                info.version = j.value("version", "");
                info.channel = j.value("channel", "");
                info.joinTime = j.value("joinTime", "");
                info.jobId = j.value("jobId", "");
                info.placeId = j.value("placeId", "");
                info.universeId = j.value("universeId", "");
                info.serverIp = j.value("serverIp", "");
                info.serverPort = j.value("serverPort", "");
                info.userId = j.value("userId", "");
                info.outputLines = j.value("outputLines", std::vector<std::string>{});
                logs.push_back(std::move(info));
            }
            LOG_INFO("Loaded " + std::to_string(logs.size()) + " log entries");
        } catch (const std::exception &e) {
            LOG_ERROR("Failed to parse " + filename + ": " + e.what());
        }

        return logs;
    }

    void SaveLogHistory(const std::vector<LogInfo> &logs, const std::string &filename) {
        std::string path = MakePath(filename);
        std::ofstream out{path};
        if (!out.is_open()) {
            LOG_ERROR("Could not open '" + path + "' for writing");
            return;
        }

        json arr = json::array();
        for (const auto &log : logs) {
            arr.push_back({
                {"fileName", log.fileName},
                {"fullPath", log.fullPath},
                {"timestamp", log.timestamp},
                {"version", log.version},
                {"channel", log.channel},
                {"joinTime", log.joinTime},
                {"jobId", log.jobId},
                {"placeId", log.placeId},
                {"universeId", log.universeId},
                {"serverIp", log.serverIp},
                {"serverPort", log.serverPort},
                {"userId", log.userId},
                {"outputLines", log.outputLines}
            });
        }

        out << arr.dump(4);
        LOG_INFO("Saved " + std::to_string(logs.size()) + " log entries");
    }

    std::string StorageFilePath(const std::string &filename) {
        return MakePath(filename);
    }
}
