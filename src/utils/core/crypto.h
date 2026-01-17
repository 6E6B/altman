#pragma once

// Prevent conflicts between OpenSSL and Windows wincrypt.h
// These must be defined before including any OpenSSL headers
#ifdef _WIN32
#	ifndef OPENSSL_NO_WINCRYPT
#		define OPENSSL_NO_WINCRYPT
#	endif
// Undefine Windows crypto macros that conflict with OpenSSL
#	ifdef X509_NAME
#		undef X509_NAME
#	endif
#	ifdef X509_EXTENSIONS
#		undef X509_EXTENSIONS
#	endif
#	ifdef PKCS7_SIGNER_INFO
#		undef PKCS7_SIGNER_INFO
#	endif
#endif

#include <memory>
#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <string>
#include <vector>

#include "base64.h"
#include "logging.hpp"

namespace Crypto {

	// Custom deleters for OpenSSL types
	struct EVP_PKEY_Deleter {
			void operator()(EVP_PKEY *pkey) const {
				if (pkey) { EVP_PKEY_free(pkey); }
			}
	};

	struct EVP_PKEY_CTX_Deleter {
			void operator()(EVP_PKEY_CTX *ctx) const {
				if (ctx) { EVP_PKEY_CTX_free(ctx); }
			}
	};

	struct EVP_MD_CTX_Deleter {
			void operator()(EVP_MD_CTX *ctx) const {
				if (ctx) { EVP_MD_CTX_free(ctx); }
			}
	};

	struct BIO_Deleter {
			void operator()(BIO *bio) const {
				if (bio) { BIO_free_all(bio); }
			}
	};

	using EVP_PKEY_Ptr = std::unique_ptr<EVP_PKEY, EVP_PKEY_Deleter>;
	using EVP_PKEY_CTX_Ptr = std::unique_ptr<EVP_PKEY_CTX, EVP_PKEY_CTX_Deleter>;
	using EVP_MD_CTX_Ptr = std::unique_ptr<EVP_MD_CTX, EVP_MD_CTX_Deleter>;
	using BIO_Ptr = std::unique_ptr<BIO, BIO_Deleter>;

	/**
	 * Represents an ECDSA P-256 key pair for HBA signing
	 */
	struct ECKeyPair {
			std::string privateKeyPEM;
			std::string publicKeyPEM;

			bool isValid() const { return !privateKeyPEM.empty(); }
	};

	/**
	 * Compute SHA-256 hash and return as Base64-encoded string
	 * @param data Input data to hash (can be empty)
	 * @return Base64-encoded SHA-256 hash
	 */
	inline std::string sha256Base64(const std::string &data) {
		unsigned char hash[SHA256_DIGEST_LENGTH];
		SHA256(reinterpret_cast<const unsigned char *>(data.c_str()), data.size(), hash);

		std::vector<BYTE> hashVec(hash, hash + SHA256_DIGEST_LENGTH);
		return base64_encode(hashVec);
	}

	/**
	 * Generate a new ECDSA P-256 key pair
	 * @return ECKeyPair with PEM-encoded keys, or empty if generation fails
	 */
	inline ECKeyPair generateECKeyPair() {
		ECKeyPair result;

		// Create context for key generation
		EVP_PKEY_CTX_Ptr ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr));
		if (!ctx) {
			LOG_ERROR("Failed to create EVP_PKEY_CTX for EC key generation");
			return result;
		}

		if (EVP_PKEY_keygen_init(ctx.get()) <= 0) {
			LOG_ERROR("Failed to initialize EC key generation");
			return result;
		}

		// Set curve to P-256 (prime256v1 / secp256r1)
		if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx.get(), NID_X9_62_prime256v1) <= 0) {
			LOG_ERROR("Failed to set EC curve to P-256");
			return result;
		}

		// Generate the key
		EVP_PKEY *pkeyRaw = nullptr;
		if (EVP_PKEY_keygen(ctx.get(), &pkeyRaw) <= 0) {
			LOG_ERROR("Failed to generate EC key pair");
			return result;
		}
		EVP_PKEY_Ptr pkey(pkeyRaw);

		// Export private key to PEM
		BIO_Ptr bioPriv(BIO_new(BIO_s_mem()));
		if (!bioPriv
			|| PEM_write_bio_PrivateKey(bioPriv.get(), pkey.get(), nullptr, nullptr, 0, nullptr, nullptr) <= 0) {
			LOG_ERROR("Failed to export private key to PEM");
			return result;
		}

		char *privData = nullptr;
		long privLen = BIO_get_mem_data(bioPriv.get(), &privData);
		if (privLen > 0 && privData) { result.privateKeyPEM = std::string(privData, privLen); }

		// Export public key to PEM
		BIO_Ptr bioPub(BIO_new(BIO_s_mem()));
		if (!bioPub || PEM_write_bio_PUBKEY(bioPub.get(), pkey.get()) <= 0) {
			LOG_ERROR("Failed to export public key to PEM");
			result.privateKeyPEM.clear();
			return result;
		}

		char *pubData = nullptr;
		long pubLen = BIO_get_mem_data(bioPub.get(), &pubData);
		if (pubLen > 0 && pubData) { result.publicKeyPEM = std::string(pubData, pubLen); }

		LOG_INFO("Successfully generated ECDSA P-256 key pair");
		return result;
	}

	/**
	 * Load a private key from PEM string
	 * @param pem PEM-encoded private key
	 * @return EVP_PKEY pointer (caller takes ownership via unique_ptr), or nullptr on failure
	 */
	inline EVP_PKEY_Ptr loadPrivateKeyFromPEM(const std::string &pem) {
		if (pem.empty()) { return nullptr; }

		BIO_Ptr bio(BIO_new_mem_buf(pem.c_str(), static_cast<int>(pem.size())));
		if (!bio) {
			LOG_ERROR("Failed to create BIO for PEM parsing");
			return nullptr;
		}

		EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
		if (!pkey) {
			LOG_ERROR("Failed to parse private key from PEM");
			return nullptr;
		}

		return EVP_PKEY_Ptr(pkey);
	}

	/**
	 * Sign data using ECDSA with SHA-256 and return Base64-encoded signature
	 * @param privateKeyPEM PEM-encoded private key
	 * @param data Data to sign
	 * @return Base64-encoded signature, or empty string on failure
	 */
	inline std::string signECDSA(const std::string &privateKeyPEM, const std::string &data) {
		EVP_PKEY_Ptr pkey = loadPrivateKeyFromPEM(privateKeyPEM);
		if (!pkey) {
			LOG_ERROR("Failed to load private key for signing");
			return "";
		}

		EVP_MD_CTX_Ptr mdCtx(EVP_MD_CTX_new());
		if (!mdCtx) {
			LOG_ERROR("Failed to create EVP_MD_CTX for signing");
			return "";
		}

		if (EVP_DigestSignInit(mdCtx.get(), nullptr, EVP_sha256(), nullptr, pkey.get()) <= 0) {
			LOG_ERROR("Failed to initialize digest sign");
			return "";
		}

		if (EVP_DigestSignUpdate(mdCtx.get(), data.c_str(), data.size()) <= 0) {
			LOG_ERROR("Failed to update digest sign");
			return "";
		}

		// Determine signature length
		size_t sigLen = 0;
		if (EVP_DigestSignFinal(mdCtx.get(), nullptr, &sigLen) <= 0) {
			LOG_ERROR("Failed to determine signature length");
			return "";
		}

		// Get the DER-encoded signature
		std::vector<unsigned char> derSig(sigLen);
		if (EVP_DigestSignFinal(mdCtx.get(), derSig.data(), &sigLen) <= 0) {
			LOG_ERROR("Failed to finalize signature");
			return "";
		}
		derSig.resize(sigLen);

		// Parse DER signature to extract r and s values
		const unsigned char *derPtr = derSig.data();
		ECDSA_SIG *ecSig = d2i_ECDSA_SIG(nullptr, &derPtr, static_cast<long>(sigLen));
		if (!ecSig) {
			LOG_ERROR("Failed to parse DER signature");
			return "";
		}

		const BIGNUM *r = nullptr;
		const BIGNUM *s = nullptr;
		ECDSA_SIG_get0(ecSig, &r, &s);

		// Convert r and s to fixed 32-byte arrays (P-256 uses 32-byte values)
		std::vector<unsigned char> rawSig(64, 0);
		int rLen = BN_num_bytes(r);
		int sLen = BN_num_bytes(s);

		// Pad r and s to 32 bytes each (big-endian)
		if (rLen <= 32) { BN_bn2bin(r, rawSig.data() + (32 - rLen)); }
		if (sLen <= 32) { BN_bn2bin(s, rawSig.data() + 32 + (32 - sLen)); }

		ECDSA_SIG_free(ecSig);

		// Base64 encode the raw signature
		std::vector<BYTE> sigVec(rawSig.begin(), rawSig.end());
		return base64_encode(sigVec);
	}

	/**
	 * Generate the x-bound-auth-token for Roblox HBA
	 * @param privateKeyPEM PEM-encoded ECDSA P-256 private key
	 * @param url Full request URL
	 * @param method HTTP method (GET, POST, etc.)
	 * @param body Request body (empty string for GET requests)
	 * @return Complete BAT token string, or empty on failure
	 */
	inline std::string generateBoundAuthToken(
		const std::string &privateKeyPEM,
		const std::string &url,
		const std::string &method,
		const std::string &body = ""
	) {
		if (privateKeyPEM.empty()) { return ""; }

		// Get current timestamp
		auto timestamp = std::to_string(std::time(nullptr));

		// Hash the body (empty string hashes to a specific value)
		std::string hashedBody = sha256Base64(body);

		// Build payload strings for signatures
		// Signature 1: hash|timestamp|url|METHOD
		std::string payload1 = hashedBody + "|" + timestamp + "|" + url + "|" + method;

		// Signature 2: |timestamp|url|METHOD (empty hash prefix)
		std::string payload2 = "|" + timestamp + "|" + url + "|" + method;

		// Sign both payloads
		std::string sig1 = signECDSA(privateKeyPEM, payload1);
		std::string sig2 = signECDSA(privateKeyPEM, payload2);

		if (sig1.empty() || sig2.empty()) {
			LOG_ERROR("Failed to generate signatures for BAT");
			return "";
		}

		// Assemble final token: v1|hash|timestamp|sig1|sig2
		return "v1|" + hashedBody + "|" + timestamp + "|" + sig1 + "|" + sig2;
	}

} // namespace Crypto
