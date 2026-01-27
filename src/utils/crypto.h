#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace Crypto {

enum class Error {
    InvalidInput,
    InitializationFailed,
    KeyDerivationFailed,
    EncryptionFailed,
    DecryptionFailed,
    AuthenticationFailed,  // Wrong password or tampered data
};

[[nodiscard]] constexpr std::string_view errorToString(Error e) {
    switch (e) {
        case Error::InvalidInput:         return "Invalid input";
        case Error::InitializationFailed: return "Crypto initialization failed";
        case Error::KeyDerivationFailed:  return "Key derivation failed";
        case Error::EncryptionFailed:     return "Encryption failed";
        case Error::DecryptionFailed:     return "Decryption failed";
        case Error::AuthenticationFailed: return "Authentication failed (wrong password or corrupted data)";
    }
    return "Unknown error";
}

inline constexpr std::size_t kSaltSize = 16;   // crypto_pwhash_SALTBYTES
inline constexpr std::size_t kNonceSize = 24;  // crypto_secretbox_NONCEBYTES
inline constexpr std::size_t kKeySize = 32;    // crypto_secretbox_KEYBYTES
inline constexpr std::size_t kMacSize = 16;    // crypto_secretbox_MACBYTES


struct EncryptedData {
    std::array<std::uint8_t, kSaltSize> salt;
    std::array<std::uint8_t, kNonceSize> nonce;
    std::vector<std::uint8_t> ciphertext;

    [[nodiscard]] std::vector<std::uint8_t> serialize() const;
    [[nodiscard]] static std::expected<EncryptedData, Error> deserialize(std::span<const std::uint8_t> data);
};

[[nodiscard]] std::expected<void, Error> initialize();

[[nodiscard]] std::expected<EncryptedData, Error> encrypt(
    std::span<const std::uint8_t> plaintext,
    const std::string& password
);

[[nodiscard]] std::expected<std::vector<std::uint8_t>, Error> decrypt(
    const EncryptedData& data,
    const std::string& password
);

[[nodiscard]] std::expected<std::vector<std::uint8_t>, Error> encrypt(
    const std::string& plaintext,
    const std::string& password
);

[[nodiscard]] std::expected<std::string, Error> decryptToString(
    const EncryptedData& data,
    const std::string& password
);

}
