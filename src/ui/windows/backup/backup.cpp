#include "backup.h"
#include "console/console.h"
#include "crypto.h"
#include "data.h"
#include "network/roblox/auth.h"
#include "network/roblox/common.h"
#include "network/roblox/games.h"
#include "network/roblox/session.h"
#include "network/roblox/social.h"
#include "ui/widgets/bottom_right_status.h"
#include "ui/widgets/modal_popup.h"
#include "utils/paths.h"
#include "utils/thread_task.h"

#include <nlohmann/json.hpp>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace {

    static std::atomic<bool> g_importInProgress = false;
    constexpr int kBackupVersion = 2;

    std::string buildBackupPath() {
        std::time_t t = std::time(nullptr);
        std::tm tm {};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d-backup.dat", &tm);
        auto path = AltMan::Paths::Backups() / buf;
        return path.string();
    }

    std::expected<std::string, Backup::Error> readFileContents(const std::string &path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return std::unexpected(Backup::Error::FileOpenFailed);
        }

        auto size = file.tellg();
        if (size < 0) {
            return std::unexpected(Backup::Error::FileReadFailed);
        }

        file.seekg(0, std::ios::beg);

        std::string contents;
        contents.resize(static_cast<std::size_t>(size));

        if (!file.read(contents.data(), size)) {
            return std::unexpected(Backup::Error::FileReadFailed);
        }

        return contents;
    }

    std::expected<void, Backup::Error> writeFileContents(const std::string &path, std::span<const std::uint8_t> data) {
        std::filesystem::path filePath(path);
        if (auto parent = filePath.parent_path(); !parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }

        std::ofstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return std::unexpected(Backup::Error::FileOpenFailed);
        }

        file.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
        file.flush();

        if (!file.good()) {
            return std::unexpected(Backup::Error::FileWriteFailed);
        }

        return {};
    }

    std::expected<nlohmann::json, Backup::Error> parseJson(const std::string &str) {
        if (!nlohmann::json::accept(str)) {
            return std::unexpected(Backup::Error::InvalidFormat);
        }
        return nlohmann::json::parse(str, nullptr, false);
    }

    Backup::Error mapCryptoError(Crypto::Error e) {
        switch (e) {
            case Crypto::Error::InvalidInput:
                return Backup::Error::EmptyPassword;
            case Crypto::Error::AuthenticationFailed:
                return Backup::Error::AuthenticationFailed;
            case Crypto::Error::EncryptionFailed:
            case Crypto::Error::KeyDerivationFailed:
            case Crypto::Error::InitializationFailed:
                return Backup::Error::EncryptionFailed;
            case Crypto::Error::DecryptionFailed:
                return Backup::Error::DecryptionFailed;
        }
        return Backup::Error::EncryptionFailed;
    }

} // namespace

namespace Backup {

    std::expected<std::string, Error> Export(const std::string &password) {
        if (password.empty()) {
            LOG_ERROR("Backup password cannot be empty");
            return std::unexpected(Error::EmptyPassword);
        }

        nlohmann::json j;

        j["version"] = kBackupVersion;
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        j["createdAt"] = static_cast<std::int64_t>(now);


        {
            auto settingsContents = readFileContents(AltMan::Paths::Config("settings.json").string());
            if (settingsContents) {
                auto parsed = parseJson(*settingsContents);
                if (!parsed) {
                    LOG_WARN("Failed to parse settings.json, exporting empty settings");
                }
                if (parsed && parsed->is_object()) {
                    j["settings"] = std::move(*parsed);
                }
            }
            if (!j.contains("settings") || !j["settings"].is_object()) {
                j["settings"] = nlohmann::json::object();
            }
        }

        {
            std::shared_lock lock(g_accountsMutex);
            nlohmann::json accounts = nlohmann::json::array();

            for (const auto &acct: g_accounts) {
                accounts.push_back({
                    {"id",         acct.id        },
                    {"cookie",     acct.cookie    },
                    {"note",       acct.note      },
                    {"isFavorite", acct.isFavorite}
                });
            }

            if (accounts.empty()) {
                return std::unexpected(Error::NoValidAccounts);
            }


            j["accounts"] = std::move(accounts);
        }

        {
            auto favoritesContents = readFileContents(AltMan::Paths::Config("favorites.json").string());
            if (favoritesContents) {
                auto parsed = parseJson(*favoritesContents);
                if (!parsed) {
                    LOG_WARN("Failed to parse settings.json, exporting empty settings");
                }
                if (parsed && parsed->is_array()) {
                    j["favorites"] = std::move(*parsed);
                }
            }
            if (!j.contains("favorites") || !j["favorites"].is_array()) {
                j["favorites"] = nlohmann::json::array();
            }
        }

        std::string plaintext;
        try {
            plaintext = j.dump();
        } catch (const std::exception &e) {
            LOG_ERROR("Failed to serialize backup JSON: {}", e.what());
            return std::unexpected(Error::SerializationFailed);
        }

        auto encrypted = Crypto::encrypt(plaintext, password);
        if (!encrypted) {
            LOG_ERROR("Failed to encrypt backup: {}", Crypto::errorToString(encrypted.error()));
            return std::unexpected(mapCryptoError(encrypted.error()));
        }

        // Plaintext backup lives in memory unprotected so we explicitly overwrite after encryption
        std::fill(plaintext.begin(), plaintext.end(), '\0');

        const std::string path = buildBackupPath();
        auto writeResult = writeFileContents(path, *encrypted);
        if (!writeResult) {
            LOG_ERROR("Failed to write backup file: {}", path);
            return std::unexpected(writeResult.error());
        }

        LOG_INFO("Backup exported to: {}", path);
        return path;
    }

    std::expected<void, Error> Import(const std::string &filePath, const std::string &password) {
        if (password.empty()) {
            return std::unexpected(Error::EmptyPassword);
        }

        auto fileContents = readFileContents(filePath);
        if (!fileContents) {
            LOG_ERROR("Failed to open backup file: {}", filePath);
            return std::unexpected(fileContents.error());
        }

        std::span<const std::uint8_t> rawData(
            reinterpret_cast<const std::uint8_t *>(fileContents->data()),
            fileContents->size()
        );

        auto encryptedData = Crypto::EncryptedData::deserialize(rawData);
        if (!encryptedData) {
            LOG_ERROR("Invalid backup file format");
            return std::unexpected(Error::InvalidFormat);
        }

        auto decrypted = Crypto::decryptToString(*encryptedData, password);
        if (!decrypted) {
            LOG_ERROR("Failed to decrypt backup: {}", Crypto::errorToString(decrypted.error()));
            return std::unexpected(mapCryptoError(decrypted.error()));
        }

        auto j = parseJson(*decrypted);
        if (!j) {
            LOG_ERROR("Failed to parse decrypted backup");
            return std::unexpected(Error::InvalidFormat);
        }

        if (!j->contains("version") || !(*j)["version"].is_number_integer()) {
            return std::unexpected(Error::UnsupportedVersion);
        }

        const int version = (*j)["version"].get<int>();
        if (version > kBackupVersion) {
            LOG_ERROR("Backup version {} is newer than supported version {}", version, kBackupVersion);
            return std::unexpected(Error::UnsupportedVersion);
        }

        if (!j->contains("accounts") || !(*j)["accounts"].is_array()) {
            return std::unexpected(Error::InvalidFormat);
        }

        std::vector<std::future<std::optional<AccountData>>> futures;
        futures.reserve((*j)["accounts"].size());

        for (const auto &item : (*j)["accounts"]) {
            if (!item.is_object()) {
                continue;
            }

            std::string cookie = item.value("cookie", "");
            if (cookie.empty()) {
                continue;
            }

            std::string note = item.value("note", "");
            bool isFavorite = item.value("isFavorite", false);
            std::uint64_t id = item.value("id", static_cast<std::uint64_t>(0));

            futures.push_back(std::async(std::launch::async, [=]() -> std::optional<AccountData> {
                if (Roblox::cachedBanStatus(cookie) == Roblox::BanCheckResult::InvalidCookie) {
                    LOG_WARN("Skipping invalid cookie during import (ID: {})", id);
                    return std::nullopt;
                }

                std::uint64_t uid = Roblox::getUserId(cookie);
                std::string username = Roblox::getUsername(cookie);
                std::string displayName = Roblox::getDisplayName(cookie);

                if (uid == 0 || username.empty() || displayName.empty()) {
                    LOG_WARN("Skipping account with invalid user data (ID: {})", id);
                    return std::nullopt;
                }

                AccountData acct;
                acct.id = id;
                acct.cookie = cookie;
                acct.note = note;
                acct.isFavorite = isFavorite;
                acct.userId = std::to_string(uid);
                acct.username = std::move(username);
                acct.displayName = std::move(displayName);

                acct.status = Roblox::getPresence(cookie, uid);

                auto vs = Roblox::getVoiceChatStatus(cookie);
                acct.voiceStatus = vs.status;
                acct.voiceBanExpiry = vs.bannedUntil;

                return acct;
            }));
        }

        std::vector<AccountData> imported;
        imported.reserve(futures.size());

        for (auto &fut : futures) {
            if (auto result = fut.get()) {
                imported.push_back(std::move(*result));
            }
        }

        if (imported.empty()) {
            return std::unexpected(Error::NoValidAccounts);
        }

        {
            std::unique_lock lock(g_accountsMutex);
            g_accounts = std::move(imported);
            invalidateAccountIndex();
        }

        if (j->contains("settings") && (*j)["settings"].is_object()) {
            std::ofstream settingsFile(AltMan::Paths::Config("settings.json"));
            if (!settingsFile.is_open()) {
                return std::unexpected(Error::SettingsWriteFailed);
            }
            settingsFile << (*j)["settings"].dump(4);
            if (!settingsFile.good()) {
                return std::unexpected(Error::SettingsWriteFailed);
            }
        }

        if (j->contains("favorites") && (*j)["favorites"].is_array()) {
            std::ofstream favoritesFile(AltMan::Paths::Config("favorites.json"));
            if (!favoritesFile.is_open()) {
                return std::unexpected(Error::FavoritesWriteFailed);
            }
            favoritesFile << (*j)["favorites"].dump(4);
            if (!favoritesFile.good()) {
                return std::unexpected(Error::FavoritesWriteFailed);
            }
        }

        Data::SaveAccounts();
        Data::LoadAccounts();
        Data::LoadSettings();
        Data::LoadFavorites();

        LOG_INFO("Successfully imported backup from: {}", filePath);
        return {};
    }

    void ImportAsync(const std::string &filePath, const std::string &password) {
        if (g_importInProgress) {
            return;
        }

        g_importInProgress = true;
        BottomRightStatus::Loading("Importing backup...");

        ThreadTask::fireAndForget([=]() {
            auto result = Import(filePath, password);

            ThreadTask::RunOnMain([result = std::move(result)]() {
                g_importInProgress = false;

                if (result) {
                    BottomRightStatus::Success("Backup imported successfully");
                } else {
                    ModalPopup::AddInfo(errorToString(result.error()).data());
                    BottomRightStatus::Clear();
                }
            });
        });
    }

    bool IsImportInProgress() {
        return g_importInProgress;
    }

} // namespace Backup
