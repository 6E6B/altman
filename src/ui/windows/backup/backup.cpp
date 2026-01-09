#include "backup.h"
#include "data.h"
#include "console/console.h"
#include "utils/threading.h"
#include "network/roblox/common.h"
#include "network/roblox/auth.h"
#include "network/roblox/games.h"
#include "network/roblox/session.h"
#include "network/roblox/social.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <ctime>

namespace {
std::string xorEncrypt(const std::string &data, const std::string &password) {
    std::string out = data;
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] ^= password[i % password.size()];
    }
    return out;
}

std::string rleCompress(const std::string &data) {
    std::string out;
    for (size_t i = 0; i < data.size();) {
        char c = data[i];
        size_t j = i;
        while (j < data.size() && data[j] == c && j - i < 255)
            ++j;
        out.push_back(c);
        out.push_back(static_cast<char>(j - i));
        i = j;
    }
    return out;
}

std::string rleDecompress(const std::string &data) {
    std::string out;
    for (size_t i = 0; i + 1 < data.size(); i += 2) {
        char c = data[i];
        auto count = static_cast<unsigned char>(data[i + 1]);
        out.append(count, c);
    }
    return out;
}

std::filesystem::path getBackupDir() {
    std::filesystem::path dir = Data::StorageFilePath("backups");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        LOG_ERROR("Failed to create backups directory: ", ec.message());
    }
    return dir;
}

std::string buildBackupPath() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d-backup.dat", &tm);
    auto path = getBackupDir() / buf;
    return path.string();
}
}

bool Backup::Export(const std::string &password) {
    nlohmann::json j;
    // settings
    {
        std::ifstream in(Data::StorageFilePath("settings.json"));
        if (in.is_open())
            in >> j["settings"];
        else
            j["settings"] = nlohmann::json::object();
    }
    // accounts
    nlohmann::json accounts = nlohmann::json::array();
    for (const auto &acct : g_accounts) {
        accounts.push_back({
            {"id", acct.id},
            {"cookie", acct.cookie},
            {"note", acct.note},
            {"isFavorite", acct.isFavorite}
        });
    }
    j["accounts"] = std::move(accounts);
    // favorites
    {
        std::ifstream in(Data::StorageFilePath("favorites.json"));
        if (in.is_open())
            in >> j["favorites"];
        else
            j["favorites"] = nlohmann::json::array();
    }
    std::string plain = j.dump();
    std::string encrypted = xorEncrypt(plain, password);
    std::string compressed = rleCompress(encrypted);
    std::string path = buildBackupPath();
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        LOG_ERROR("Failed to write backup");
        return false;
    }
    out.write(compressed.data(), compressed.size());
    return true;
}

bool Backup::Import(const std::string &file, const std::string &password, std::string *error) {
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) {
        LOG_ERROR("Failed to open backup file");
        return false;
    }
    std::string compressed((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string decrypted = xorEncrypt(rleDecompress(compressed), password);

    nlohmann::json j;
    j = nlohmann::json::parse(decrypted);
    if (!j.contains("accounts") || !j["accounts"].is_array()) {
        LOG_ERROR("Failed to decrypt. Invalid password.");
        if (error && error->empty()) *error = "Invalid backup format";
        return false;
    }

    g_accounts.clear();
    for (auto &item : j["accounts"]) {
        AccountData acct;
        acct.id = item.value("id", 0);
        acct.cookie = item.value("cookie", "");
        acct.note = item.value("note", "");
        acct.isFavorite = item.value("isFavorite", false);
        
        // Validate cookie first to avoid multiple error messages
        Roblox::BanCheckResult banStatus = Roblox::cachedBanStatus(acct.cookie);
        if (banStatus == Roblox::BanCheckResult::InvalidCookie) {
            LOG_WARN("Skipping account with invalid cookie during backup import (ID: {})", acct.id);
            continue;
        }
        
        // Get user information
        uint64_t uid = Roblox::getUserId(acct.cookie);
        std::string username = Roblox::getUsername(acct.cookie);
        std::string displayName = Roblox::getDisplayName(acct.cookie);
        
        // Double-check that we got valid user data
        if (uid == 0 || username.empty() || displayName.empty()) {
            LOG_WARN("Skipping account with invalid data during backup import (ID: {})", acct.id);
            continue;
        }
        
        acct.userId = std::to_string(uid);
        acct.username = username;
        acct.displayName = displayName;
        acct.status = Roblox::getPresence(acct.cookie, uid);
        auto vs = Roblox::getVoiceChatStatus(acct.cookie);
        acct.voiceStatus = vs.status;
        acct.voiceBanExpiry = vs.bannedUntil;
        g_accounts.push_back(std::move(acct));
    }
    if (j.contains("settings")) {
        std::ofstream s(Data::StorageFilePath("settings.json"));
        s << j["settings"].dump(4);
    }
    if (j.contains("favorites")) {
        std::ofstream f(Data::StorageFilePath("favorites.json"));
        f << j["favorites"].dump(4);
    }
    Data::SaveAccounts();
    Data::LoadAccounts();
    Data::LoadSettings();
    Data::LoadFavorites();
    return true;
}
