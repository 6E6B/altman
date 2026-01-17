#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace Roblox { namespace HBA {

	/**
	 * Represents a whitelisted API site and its sample rate for BAT enforcement
	 */
	struct APISiteWhitelistItem {
			std::string apiSite;
			int sampleRate = 100; // Percentage (0-100)
	};

	/**
	 * Represents an exempted API site that never requires BAT
	 */
	struct APISiteExemptItem {
			std::string apiSite;
	};

	/**
	 * Metadata fetched from Roblox for HBA configuration
	 */
	struct TokenMetadata {
			bool isSecureAuthenticationIntentEnabled = false;
			bool isBoundAuthTokenEnabledForAllUrls = false;
			std::vector<APISiteWhitelistItem> whitelist;
			std::vector<APISiteExemptItem> exemptlist;

			// IndexedDB configuration (informational, not used in native app)
			std::string hbaIndexedDbName;
			std::string hbaIndexedDbObjStoreName;
			std::string hbaIndexedDbKeyName;
			int hbaIndexedDbVersion = 1;

			// Whether the session appears authenticated
			bool isAuthenticated = false;

			// Cache management
			std::chrono::steady_clock::time_point fetchedAt;

			bool isValid() const {
				return isSecureAuthenticationIntentEnabled || isBoundAuthTokenEnabledForAllUrls || !whitelist.empty();
			}

			bool isExpired(std::chrono::minutes ttl = std::chrono::minutes(5)) const {
				auto now = std::chrono::steady_clock::now();
				return (now - fetchedAt) > ttl;
			}
	};

	/**
	 * ECDSA P-256 key pair for BAT signing
	 */
	struct KeyPair {
			std::string privateKeyPEM; // PEM-encoded private key (stored encrypted)
			std::string publicKeyPEM; // PEM-encoded public key (for reference)

			bool isValid() const { return !privateKeyPEM.empty(); }
	};

	/**
	 * Configuration for authenticated requests with HBA support
	 */
	struct AuthConfig {
			std::string cookie; // .ROBLOSECURITY cookie value
			std::string hbaPrivateKey; // PEM-encoded private key (empty = skip BAT)
			bool hbaEnabled = true; // Whether to attempt BAT generation

			bool hasHBA() const { return hbaEnabled && !hbaPrivateKey.empty(); }
	};

	/**
	 * Constants for BAT generation
	 */
	namespace Constants {
		// Token format version
		constexpr const char *BAT_VERSION = "v1";

		// Header name for bound auth token
		constexpr const char *TOKEN_HEADER_NAME = "x-bound-auth-token";

		// Separator used in token format
		constexpr char TOKEN_SEPARATOR = '|';

		// URL to fetch metadata from
		constexpr const char *METADATA_URL = "https://www.roblox.com/charts";

		// Base URL pattern for Roblox APIs
		constexpr const char *ROBLOX_URL_BASE = ".roblox.com";

		// URLs that always require BAT regardless of whitelist
		inline const std::vector<std::string> FORCE_BAT_URLS = {
			"/account-switcher/v1/switch",
		};

		// Default cache TTL for metadata
		constexpr int METADATA_CACHE_TTL_MINUTES = 5;

		// HTML selectors/regex patterns for metadata parsing
		constexpr const char *META_SELECTOR = "hardware-backed-authentication-data";
		constexpr const char *USER_DATA_SELECTOR = "user-data";
	} // namespace Constants

}} // namespace Roblox::HBA
