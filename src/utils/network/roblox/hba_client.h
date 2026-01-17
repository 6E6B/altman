#pragma once

#include <algorithm>
#include <chrono>
#include <mutex>
#include <random>
#include <regex>
#include <string>

#include "core/crypto.h"
#include "core/logging.hpp"
#include "hba.h"
#include "http.hpp"

namespace Roblox { namespace HBA {

	/**
	 * Client for handling Hardware-Backed Authentication (HBA) / Bound Auth Token (BAT)
	 * generation for Roblox API requests.
	 */
	class Client {
		public:
			Client() = default;
			~Client() = default;

			// Non-copyable
			Client(const Client &) = delete;
			Client &operator=(const Client &) = delete;

			/**
			 * Get the singleton instance of the HBA client
			 */
			static Client &instance() {
				static Client instance;
				return instance;
			}

			/**
			 * Fetch and cache token metadata from Roblox
			 * @param cookie Optional cookie for authenticated metadata fetch
			 * @param forceRefresh Force refresh even if cache is valid
			 * @return TokenMetadata structure with configuration
			 */
			TokenMetadata getTokenMetadata(const std::string &cookie = "", bool forceRefresh = false) {
				std::lock_guard<std::mutex> lock(m_metadataMutex);

				// Return cached if valid and not forcing refresh
				if (!forceRefresh && m_cachedMetadata.isValid() && !m_cachedMetadata.isExpired()) {
					return m_cachedMetadata;
				}

				// Fetch fresh metadata
				TokenMetadata metadata = fetchMetadataFromServer(cookie);
				if (metadata.isValid()) { m_cachedMetadata = metadata; }

				return m_cachedMetadata;
			}

			/**
			 * Check if a URL requires a bound auth token
			 * @param url The request URL
			 * @param isAuthenticated Whether the request includes credentials
			 * @param cookie Optional cookie for metadata fetch
			 * @return true if BAT should be generated
			 */
			bool isUrlProtected(const std::string &url, bool isAuthenticated = true, const std::string &cookie = "") {
				// Must be a Roblox URL
				if (url.find(Constants::ROBLOX_URL_BASE) == std::string::npos) { return false; }

				// Check forced BAT URLs first
				for (const auto &forcedUrl : Constants::FORCE_BAT_URLS) {
					if (url.find(forcedUrl) != std::string::npos) { return true; }
				}

				// Get metadata
				TokenMetadata metadata = getTokenMetadata(cookie, false);

				// Must be authenticated for most protected endpoints
				if (!isAuthenticated && !metadata.isAuthenticated) { return false; }

				// Check exemptlist first (never requires BAT)
				for (const auto &exempt : metadata.exemptlist) {
					if (url.find(exempt.apiSite) != std::string::npos) { return false; }
				}

				// If enabled for all URLs, it's protected
				if (metadata.isBoundAuthTokenEnabledForAllUrls) { return true; }

				// Check whitelist with sample rate
				for (const auto &item : metadata.whitelist) {
					if (url.find(item.apiSite) != std::string::npos) {
						// Sample rate check (0-100)
						if (item.sampleRate >= 100) { return true; }
						if (item.sampleRate > 0) {
							std::random_device rd;
							std::mt19937 gen(rd());
							std::uniform_int_distribution<> dis(0, 99);
							return dis(gen) < item.sampleRate;
						}
					}
				}

				return false;
			}

			/**
			 * Generate the x-bound-auth-token header value
			 * @param privateKeyPEM PEM-encoded ECDSA P-256 private key
			 * @param url Request URL
			 * @param method HTTP method (GET, POST, etc.)
			 * @param body Request body (empty for GET)
			 * @return Token string or empty if generation fails
			 */
			std::string generateToken(
				const std::string &privateKeyPEM,
				const std::string &url,
				const std::string &method,
				const std::string &body = ""
			) {
				return Crypto::generateBoundAuthToken(privateKeyPEM, url, method, body);
			}

			/**
			 * Generate headers map with x-bound-auth-token if needed
			 * @param config Authentication configuration
			 * @param url Request URL
			 * @param method HTTP method
			 * @param body Request body
			 * @return Map with x-bound-auth-token header if applicable
			 */
			std::map<std::string, std::string> generateBATHeaders(
				const AuthConfig &config,
				const std::string &url,
				const std::string &method,
				const std::string &body = ""
			) {
				std::map<std::string, std::string> headers;

				if (!config.hasHBA()) { return headers; }

				if (!isUrlProtected(url, true, config.cookie)) { return headers; }

				std::string token = generateToken(config.hbaPrivateKey, url, method, body);
				if (!token.empty()) {
					headers[Constants::TOKEN_HEADER_NAME] = token;
					LOG_INFO("Generated BAT for: " + url);
				} else {
					LOG_ERROR("Failed to generate BAT for: " + url);
				}

				return headers;
			}

			/**
			 * Clear cached metadata, forcing a refresh on next request
			 */
			void clearCache() {
				std::lock_guard<std::mutex> lock(m_metadataMutex);
				m_cachedMetadata = TokenMetadata();
			}

		private:
			std::mutex m_metadataMutex;
			TokenMetadata m_cachedMetadata;

			/**
			 * Fetch metadata from Roblox server
			 */
			TokenMetadata fetchMetadataFromServer(const std::string &cookie) {
				TokenMetadata metadata;
				metadata.fetchedAt = std::chrono::steady_clock::now();

				LOG_INFO("Fetching HBA metadata from Roblox");

				HttpClient::Response response;
				if (!cookie.empty()) {
					response = HttpClient::get(
						Constants::METADATA_URL,
						{
							{"Cookie", ".ROBLOSECURITY=" + cookie}
					}
					);
				} else {
					response = HttpClient::get(Constants::METADATA_URL, {});
				}

				if (response.status_code < 200 || response.status_code >= 300) {
					LOG_ERROR("Failed to fetch HBA metadata: HTTP " + std::to_string(response.status_code));
					return metadata;
				}

				// Parse the HTML to extract metadata
				parseMetadataFromHTML(response.text, metadata);

				if (metadata.isValid()) { LOG_INFO("Successfully fetched HBA metadata"); }

				return metadata;
			}

			/**
			 * Parse metadata from HTML response
			 */
			void parseMetadataFromHTML(const std::string &html, TokenMetadata &metadata) {
				// Check for user-data meta tag to determine authentication status
				metadata.isAuthenticated = (html.find("name=\"user-data\"") != std::string::npos);

				// Find the hardware-backed-authentication-data meta tag
				size_t metaStart = html.find("name=\"hardware-backed-authentication-data\"");
				if (metaStart == std::string::npos) {
					LOG_ERROR("HBA meta tag not found in response");
					return;
				}

				// Find the surrounding meta tag
				size_t tagStart = html.rfind("<meta", metaStart);
				size_t tagEnd = html.find(">", metaStart);
				if (tagStart == std::string::npos || tagEnd == std::string::npos) { return; }

				std::string metaTag = html.substr(tagStart, tagEnd - tagStart + 1);

				// Extract attributes
				metadata.isSecureAuthenticationIntentEnabled
					= extractBoolAttribute(metaTag, "data-is-secure-authentication-intent-enabled");

				metadata.isBoundAuthTokenEnabledForAllUrls
					= extractBoolAttribute(metaTag, "data-is-bound-auth-token-enabled");

				// Extract whitelist JSON
				std::string whitelistJson = extractAttribute(metaTag, "data-bound-auth-token-whitelist");
				if (!whitelistJson.empty()) { parseWhitelist(decodeHtmlEntities(whitelistJson), metadata.whitelist); }

				// Extract exemptlist JSON
				std::string exemptlistJson = extractAttribute(metaTag, "data-bound-auth-token-exemptlist");
				if (!exemptlistJson.empty()) {
					parseExemptlist(decodeHtmlEntities(exemptlistJson), metadata.exemptlist);
				}

				// Extract IndexedDB config (informational)
				metadata.hbaIndexedDbName = extractAttribute(metaTag, "data-hba-indexed-db-name");
				metadata.hbaIndexedDbObjStoreName = extractAttribute(metaTag, "data-hba-indexed-db-obj-store-name");
				metadata.hbaIndexedDbKeyName = extractAttribute(metaTag, "data-hba-indexed-db-key-name");

				std::string versionStr = extractAttribute(metaTag, "data-hba-indexed-db-version");
				if (!versionStr.empty()) {
					try {
						metadata.hbaIndexedDbVersion = std::stoi(versionStr);
					} catch (...) { metadata.hbaIndexedDbVersion = 1; }
				}
			}

			/**
			 * Extract an attribute value from an HTML tag
			 */
			std::string extractAttribute(const std::string &tag, const std::string &attrName) {
				std::string pattern = attrName + "=\"";
				size_t start = tag.find(pattern);
				if (start == std::string::npos) { return ""; }

				start += pattern.length();
				size_t end = tag.find("\"", start);
				if (end == std::string::npos) { return ""; }

				return tag.substr(start, end - start);
			}

			/**
			 * Extract a boolean attribute value
			 */
			bool extractBoolAttribute(const std::string &tag, const std::string &attrName) {
				std::string value = extractAttribute(tag, attrName);
				return value == "true" || value == "True" || value == "TRUE";
			}

			/**
			 * Decode HTML entities in a string
			 */
			std::string decodeHtmlEntities(const std::string &input) {
				std::string output = input;

				// Common HTML entities
				const std::vector<std::pair<std::string, std::string>> entities = {
					{"&quot;", "\""},
					{"&amp;",  "&" },
					{"&lt;",	 "<" },
					{"&gt;",	 ">" },
					{"&nbsp;", " " },
				};

				for (const auto &entity : entities) {
					size_t pos = 0;
					while ((pos = output.find(entity.first, pos)) != std::string::npos) {
						output.replace(pos, entity.first.length(), entity.second);
						pos += entity.second.length();
					}
				}

				// Numeric entities (&#123; format)
				std::regex numericEntity("&#(\\d+);");
				std::smatch match;
				while (std::regex_search(output, match, numericEntity)) {
					int codePoint = std::stoi(match[1].str());
					std::string replacement(1, static_cast<char>(codePoint));
					output.replace(match.position(), match.length(), replacement);
				}

				return output;
			}

			/**
			 * Parse whitelist JSON into vector
			 */
			void parseWhitelist(const std::string &json, std::vector<APISiteWhitelistItem> &whitelist) {
				// Simple JSON parsing for {"Whitelist":[{"apiSite":"...","sampleRate":"100"},...]}
				size_t arrayStart = json.find("[");
				size_t arrayEnd = json.rfind("]");
				if (arrayStart == std::string::npos || arrayEnd == std::string::npos) { return; }

				std::string arrayContent = json.substr(arrayStart + 1, arrayEnd - arrayStart - 1);

				// Parse each object
				size_t pos = 0;
				while ((pos = arrayContent.find("{", pos)) != std::string::npos) {
					size_t objEnd = arrayContent.find("}", pos);
					if (objEnd == std::string::npos) { break; }

					std::string obj = arrayContent.substr(pos, objEnd - pos + 1);

					APISiteWhitelistItem item;
					item.apiSite = extractJsonString(obj, "apiSite");

					std::string rateStr = extractJsonString(obj, "sampleRate");
					if (!rateStr.empty()) {
						try {
							item.sampleRate = std::stoi(rateStr);
						} catch (...) { item.sampleRate = 100; }
					}

					if (!item.apiSite.empty()) { whitelist.push_back(item); }

					pos = objEnd + 1;
				}
			}

			/**
			 * Parse exemptlist JSON into vector
			 */
			void parseExemptlist(const std::string &json, std::vector<APISiteExemptItem> &exemptlist) {
				// Simple JSON parsing for {"Exemptlist":[{"apiSite":"..."},...]}
				size_t arrayStart = json.find("[");
				size_t arrayEnd = json.rfind("]");
				if (arrayStart == std::string::npos || arrayEnd == std::string::npos) { return; }

				std::string arrayContent = json.substr(arrayStart + 1, arrayEnd - arrayStart - 1);

				size_t pos = 0;
				while ((pos = arrayContent.find("{", pos)) != std::string::npos) {
					size_t objEnd = arrayContent.find("}", pos);
					if (objEnd == std::string::npos) { break; }

					std::string obj = arrayContent.substr(pos, objEnd - pos + 1);

					APISiteExemptItem item;
					item.apiSite = extractJsonString(obj, "apiSite");

					if (!item.apiSite.empty()) { exemptlist.push_back(item); }

					pos = objEnd + 1;
				}
			}

			/**
			 * Extract a string value from simple JSON object
			 */
			std::string extractJsonString(const std::string &json, const std::string &key) {
				std::string pattern = "\"" + key + "\":\"";
				size_t start = json.find(pattern);
				if (start == std::string::npos) {
					// Try without quotes around value (for numbers stored as strings)
					pattern = "\"" + key + "\":";
					start = json.find(pattern);
					if (start == std::string::npos) { return ""; }
					start += pattern.length();

					// Skip whitespace
					while (start < json.length() && (json[start] == ' ' || json[start] == '"')) { start++; }

					size_t end = start;
					while (end < json.length() && json[end] != ',' && json[end] != '}' && json[end] != '"') { end++; }
					return json.substr(start, end - start);
				}

				start += pattern.length();
				size_t end = json.find("\"", start);
				if (end == std::string::npos) { return ""; }

				return json.substr(start, end - start);
			}
	};

	/**
	 * Convenience function to get the global HBA client instance
	 */
	inline Client &getClient() { return Client::instance(); }

}} // namespace Roblox::HBA
