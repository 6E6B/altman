#include "crypto.h"

#include <sodium.h>

#include <algorithm>

namespace Crypto {

std::vector<std::uint8_t> EncryptedData::serialize() const {
    std::vector<std::uint8_t> result;
    result.reserve(kSaltSize + kNonceSize + ciphertext.size());

    result.insert(result.end(), salt.begin(), salt.end());
    result.insert(result.end(), nonce.begin(), nonce.end());
    result.insert(result.end(), ciphertext.begin(), ciphertext.end());

    return result;
}

std::expected<EncryptedData, Error> EncryptedData::deserialize(std::span<const std::uint8_t> data) {
    constexpr std::size_t kHeaderSize = kSaltSize + kNonceSize;

    if (data.size() < kHeaderSize + kMacSize) {
        return std::unexpected(Error::InvalidInput);
    }

    EncryptedData result;

    std::size_t offset = 0;
    std::copy_n(data.data() + offset, kSaltSize, result.salt.begin());
    offset += kSaltSize;

    std::copy_n(data.data() + offset, kNonceSize, result.nonce.begin());
    offset += kNonceSize;

    result.ciphertext.assign(
        data.begin() + static_cast<std::ptrdiff_t>(offset),
        data.end()
    );

    return result;
}

std::expected<void, Error> initialize() {
    if (sodium_init() < 0) {
        return std::unexpected(Error::InitializationFailed);
    }
    return {};
}

namespace {

std::expected<std::array<std::uint8_t, kKeySize>, Error> deriveKey(
    const std::string& password,
    std::span<const std::uint8_t, kSaltSize> salt
) {
    std::array<std::uint8_t, kKeySize> key{};

    int result = crypto_pwhash(
        key.data(),
        key.size(),
        password.data(),
        password.size(),
        salt.data(),
        crypto_pwhash_OPSLIMIT_INTERACTIVE,
        crypto_pwhash_MEMLIMIT_INTERACTIVE,
        crypto_pwhash_ALG_ARGON2ID13
    );

    if (result != 0) {
        return std::unexpected(Error::KeyDerivationFailed);
    }

    return key;
}

}

std::expected<EncryptedData, Error> encrypt(
    std::span<const std::uint8_t> plaintext,
    const std::string& password
) {
    if (password.empty()) {
        return std::unexpected(Error::InvalidInput);
    }

    EncryptedData result;

    randombytes_buf(result.salt.data(), result.salt.size());
    randombytes_buf(result.nonce.data(), result.nonce.size());

    auto keyResult = deriveKey(password, std::span<const std::uint8_t, kSaltSize>(result.salt));
    if (!keyResult) {
        return std::unexpected(keyResult.error());
    }
    const auto& key = *keyResult;

    result.ciphertext.resize(plaintext.size() + crypto_secretbox_MACBYTES);

    int ret = crypto_secretbox_easy(
        result.ciphertext.data(),
        plaintext.data(),
        plaintext.size(),
        result.nonce.data(),
        key.data()
    );

    sodium_memzero(const_cast<std::uint8_t*>(key.data()), key.size());

    if (ret != 0) {
        return std::unexpected(Error::EncryptionFailed);
    }

    return result;
}

std::expected<std::vector<std::uint8_t>, Error> decrypt(
    const EncryptedData& data,
    const std::string& password
) {
    if (password.empty()) {
        return std::unexpected(Error::InvalidInput);
    }

    if (data.ciphertext.size() < crypto_secretbox_MACBYTES) {
        return std::unexpected(Error::InvalidInput);
    }

    auto keyResult = deriveKey(password, std::span<const std::uint8_t, kSaltSize>(data.salt));
    if (!keyResult) {
        return std::unexpected(keyResult.error());
    }
    const auto& key = *keyResult;

    std::vector<std::uint8_t> plaintext(data.ciphertext.size() - crypto_secretbox_MACBYTES);

    int ret = crypto_secretbox_open_easy(
        plaintext.data(),
        data.ciphertext.data(),
        data.ciphertext.size(),
        data.nonce.data(),
        key.data()
    );

    sodium_memzero(const_cast<std::uint8_t*>(key.data()), key.size());

    if (ret != 0) {
        return std::unexpected(Error::AuthenticationFailed);
    }

    return plaintext;
}

std::expected<std::vector<std::uint8_t>, Error> encrypt(
    const std::string& plaintext,
    const std::string& password
) {
    auto result = encrypt(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(plaintext.data()),
            plaintext.size()
        ),
        password
    );

    if (!result) {
        return std::unexpected(result.error());
    }

    return result->serialize();
}

std::expected<std::string, Error> decryptToString(
    const EncryptedData& data,
    const std::string& password
) {
    auto result = decrypt(data, password);
    if (!result) {
        return std::unexpected(result.error());
    }

    return std::string(
        reinterpret_cast<const char*>(result->data()),
        result->size()
    );
}

}
