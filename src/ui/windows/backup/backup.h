#pragma once

#include <cstdint>
#include <expected>
#include <future>
#include <string>

namespace Backup {

    enum class Error {
        EmptyPassword,
        FileOpenFailed,
        FileReadFailed,
        FileWriteFailed,
        SerializationFailed,
        EncryptionFailed,
        DecryptionFailed,
        InvalidFormat,
        UnsupportedVersion,
        CorruptedData,
        AuthenticationFailed,
        NoValidAccounts,
        SettingsWriteFailed,
        FavoritesWriteFailed,
    };

    [[nodiscard]] constexpr std::string_view errorToString(Error e) {
        switch (e) {
            case Error::EmptyPassword:
                return "Password cannot be empty";
            case Error::FileOpenFailed:
                return "Failed to open file";
            case Error::FileReadFailed:
                return "Failed to read file";
            case Error::FileWriteFailed:
                return "Failed to write file";
            case Error::SerializationFailed:
                return "Failed to serialize data";
            case Error::EncryptionFailed:
                return "Encryption failed";
            case Error::DecryptionFailed:
                return "Decryption failed";
            case Error::InvalidFormat:
                return "Invalid backup format";
            case Error::UnsupportedVersion:
                return "Unsupported backup version";
            case Error::CorruptedData:
                return "Backup data is corrupted";
            case Error::AuthenticationFailed:
                return "Invalid password or corrupted backup";
            case Error::NoValidAccounts:
                return "No valid accounts found in backup";
            case Error::SettingsWriteFailed:
                return "Failed to write settings";
            case Error::FavoritesWriteFailed:
                return "Failed to write favorites";
        }
        return "Unknown error";
    }

    [[nodiscard]] std::expected<std::string, Error> Export(const std::string &password);

    void ImportAsync(const std::string &filePath, const std::string &password);

    [[nodiscard]] bool IsImportInProgress();

} // namespace Backup
