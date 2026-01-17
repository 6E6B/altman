#pragma once

#include <condition_variable>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "auth.h"
#include "authenticated_http.h"
#include "core/logging.hpp"
#include "hba.h"
#include "hba_client.h"
#include "http.hpp"
#include "threading.h"

#include "../../components/components.h"

namespace Roblox {
	/**
	 * Batch fetch user profiles (names) from the User Profile API.
	 * This is needed because the Friends API no longer returns name/displayName fields.
	 * @param userIds Vector of user IDs to fetch (will be batched in groups of 100)
	 * @return Map of userId -> combinedName
	 */
	inline std::unordered_map<uint64_t, std::string> getUserProfiles(const std::vector<uint64_t> &userIds) {
		std::unordered_map<uint64_t, std::string> result;
		if (userIds.empty()) { return result; }

		const size_t batchSize = 100;
		for (size_t i = 0; i < userIds.size(); i += batchSize) {
			size_t end = (std::min)(userIds.size(), i + batchSize);
			std::vector<uint64_t> batch(userIds.begin() + i, userIds.begin() + end);

			nlohmann::json payload = {
				{"fields",  {"names.combinedName", "names.username"}},
				{"userIds", batch								   }
			};

			auto resp = HttpClient::post(
				"https://apis.roblox.com/user-profile-api/v1/user/profiles/get-profiles",
				{
					{"Content-Type", "application/json"},
					{"Accept",	   "application/json"}
			},
				payload.dump()
			);

			if (resp.status_code < 200 || resp.status_code >= 300) {
				LOG_ERROR("Failed to fetch user profiles: HTTP " + std::to_string(resp.status_code));
				continue;
			}

			try {
				nlohmann::json j = HttpClient::decode(resp);
				if (j.contains("profileDetails") && j["profileDetails"].is_array()) {
					for (const auto &profile : j["profileDetails"]) {
						uint64_t uid = profile.value("userId", 0ULL);
						if (uid == 0) { continue; }

						std::string combinedName;
						std::string username;
						if (profile.contains("names") && profile["names"].is_object()) {
							const auto &names = profile["names"];
							if (names.contains("combinedName") && names["combinedName"].is_string()) {
								combinedName = names["combinedName"].get<std::string>();
							}
							if (names.contains("username") && names["username"].is_string()) {
								username = names["username"].get<std::string>();
							}
						}

						// Store as "combinedName|username" so we can split later
						// If username is empty, just store combinedName twice
						if (username.empty()) { username = combinedName; }
						result[uid] = combinedName + "|" + username;
					}
				}
			} catch (const std::exception &e) { LOG_ERROR(std::string("Error parsing user profiles: ") + e.what()); }
		}

		return result;
	}

	static std::vector<FriendInfo> getFriends(const std::string &userId, const std::string &cookie) {
		if (!canUseCookie(cookie)) { return {}; }

		LOG_INFO("Fetching friends list");

		HttpClient::Response resp = HttpClient::get(
			"https://friends.roblox.com/v1/users/" + userId + "/friends",
			{
				{"Cookie", ".ROBLOSECURITY=" + cookie}
		}
		);

		if (resp.status_code < 200 || resp.status_code >= 300) {
			LOG_ERROR("Failed to fetch friends: HTTP " + std::to_string(resp.status_code));
			return {};
		}

		nlohmann::json j = HttpClient::decode(resp);
		std::vector<FriendInfo> friends;
		std::vector<uint64_t> friendIds;

		if (j.contains("data") && j["data"].is_array()) {
			for (const auto &item : j["data"]) {
				FriendInfo f;
				f.id = item.value("id", 0ULL);
				// These fields are now empty from the API, but we still try to read them as fallback
				f.displayName = item.value("displayName", "");
				f.username = item.value("name", "");
				friends.push_back(f);
				if (f.id != 0) { friendIds.push_back(f.id); }
			}
		}

		// Fetch names from User Profile API since Friends API no longer returns them
		if (!friendIds.empty()) {
			LOG_INFO("Fetching friend names from User Profile API...");
			auto profiles = getUserProfiles(friendIds);
			for (auto &f : friends) {
				auto it = profiles.find(f.id);
				if (it != profiles.end()) {
					// Format is "combinedName|username"
					size_t sep = it->second.find('|');
					if (sep != std::string::npos) {
						f.displayName = it->second.substr(0, sep);
						f.username = it->second.substr(sep + 1);
					} else {
						f.displayName = it->second;
						f.username = it->second;
					}
				} else if (f.displayName.empty() && f.username.empty()) {
					// Fallback to user ID if profile fetch failed
					f.displayName = std::to_string(f.id);
					f.username = std::to_string(f.id);
				}
			}
		}

		return friends;
	}

	static FriendInfo getUserInfo(const std::string &userId) {
		LOG_INFO("Fetching user info");
		HttpClient::Response resp = HttpClient::get(
			"https://users.roblox.com/v1/users/" + userId,
			{
				{"Accept", "application/json"}
		}
		);

		if (resp.status_code < 200 || resp.status_code >= 300) {
			LOG_ERROR("Failed to fetch user info: HTTP " + std::to_string(resp.status_code));
			return FriendInfo {};
		}

		nlohmann::json j = HttpClient::decode(resp);
		FriendInfo f;
		if (!j.is_null()) {
			f.id = j.value("id", 0ULL);
			f.username = j.value("name", "");
			f.displayName = j.value("displayName", "");
		}
		return f;
	}

	struct FriendDetail {
			uint64_t id = 0;
			std::string username;
			std::string displayName;
			std::string description;
			std::string createdIso;
			int friends = 0;
			int followers = 0;
			int following = 0;
			int placeVisits = 0;
			std::string presence;
	};

	static FriendDetail getUserDetails(const std::string &userId, const std::string &cookie) {
		if (!canUseCookie(cookie)) { return FriendDetail {}; }

		FriendDetail d;
		std::mutex m;
		std::condition_variable cv;
		int remaining = 4;

		auto signalDone = [&] {
			std::lock_guard<std::mutex> lk(m);
			if (--remaining == 0) { cv.notify_one(); }
		};

		Threading::newThread([&, userId] {
			auto resp = HttpClient::get(
				"https://users.roblox.com/v1/users/" + userId,
				{
					{"Accept", "application/json"}
			}
			);
			if (resp.status_code >= 200 && resp.status_code < 300) {
				nlohmann::json j = HttpClient::decode(resp);
				d.id = j.value("id", 0ULL);
				d.username = j.value("name", "");
				d.displayName = j.value("displayName", "");
				d.description = j.value("description", "");
				d.createdIso = j.value("created", "");
			}
			signalDone();
		});

		Threading::newThread([&, userId] {
			auto resp = HttpClient::get("https://friends.roblox.com/v1/users/" + userId + "/followers/count", {});
			if (resp.status_code >= 200 && resp.status_code < 300) {
				try {
					d.followers = nlohmann::json::parse(resp.text).value("count", 0);
				} catch (const std::exception &e) {
					LOG_ERROR(std::string("Failed to parse followers count: ") + e.what());
				}
			}
			signalDone();
		});

		Threading::newThread([&, userId] {
			auto resp = HttpClient::get("https://friends.roblox.com/v1/users/" + userId + "/followings/count", {});
			if (resp.status_code >= 200 && resp.status_code < 300) {
				try {
					d.following = nlohmann::json::parse(resp.text).value("count", 0);
				} catch (const std::exception &e) {
					LOG_ERROR(std::string("Failed to parse following count: ") + e.what());
				}
			}
			signalDone();
		});

		Threading::newThread([&, userId] {
			auto resp = HttpClient::get("https://friends.roblox.com/v1/users/" + userId + "/friends/count", {});
			if (resp.status_code >= 200 && resp.status_code < 300) {
				try {
					d.friends = nlohmann::json::parse(resp.text).value("count", 0);
				} catch (const std::exception &e) {
					LOG_ERROR(std::string("Failed to parse friends count: ") + e.what());
				}
			}
			signalDone();
		});

		std::unique_lock<std::mutex> lk(m);
		cv.wait(lk, [&] { return remaining == 0; });

		return d;
	}

	struct IncomingFriendRequest {
			uint64_t userId = 0;
			std::string username;
			std::string displayName;
			std::string sentAt;
			std::vector<std::string> mutuals;
			std::string originSourceType;
			uint64_t sourceUniverseId = 0;
	};

	struct FriendRequestsPage {
			std::vector<IncomingFriendRequest> data;
			std::string nextCursor;
			std::string prevCursor;
	};

	inline FriendRequestsPage
		getIncomingFriendRequests(const std::string &cookie, const std::string &cursor = {}, int limit = 100) {
		FriendRequestsPage page;
		if (!canUseCookie(cookie)) { return page; }

		std::string url = "https://friends.roblox.com/v1/my/friends/requests?limit=" + std::to_string(limit);
		if (!cursor.empty()) { url += "&cursor=" + cursor; }

		HttpClient::Response resp = HttpClient::get(
			url,
			{
				{"Cookie", ".ROBLOSECURITY=" + cookie}
		}
		);
		if (resp.status_code < 200 || resp.status_code >= 300) {
			LOG_ERROR("Failed to fetch incoming friend requests: HTTP " + std::to_string(resp.status_code));
			return page;
		}

		nlohmann::json j = HttpClient::decode(resp);
		try {
			if (j.contains("nextPageCursor") && j["nextPageCursor"].is_string()) {
				page.nextCursor = j["nextPageCursor"].get<std::string>();
			}
			if (j.contains("previousPageCursor") && j["previousPageCursor"].is_string()) {
				page.prevCursor = j["previousPageCursor"].get<std::string>();
			}

			std::vector<uint64_t> requestUserIds;

			if (j.contains("data") && j["data"].is_array()) {
				for (const auto &it : j["data"]) {
					IncomingFriendRequest r;

					if (it.contains("id") && it["id"].is_number_unsigned()) { r.userId = it["id"].get<uint64_t>(); }

					// These fields are now empty from the API, but we still try to read them as fallback
					if (it.contains("name") && it["name"].is_string()) { r.username = it["name"].get<std::string>(); }

					if (it.contains("displayName") && it["displayName"].is_string()) {
						r.displayName = it["displayName"].get<std::string>();
					}

					if (it.contains("friendRequest") && it["friendRequest"].is_object()) {
						const auto &fr = it["friendRequest"];
						if (fr.contains("sentAt") && fr["sentAt"].is_string()) {
							r.sentAt = fr["sentAt"].get<std::string>();
						}
						if (fr.contains("originSourceType") && fr["originSourceType"].is_string()) {
							r.originSourceType = fr["originSourceType"].get<std::string>();
						}
						if (fr.contains("sourceUniverseId") && fr["sourceUniverseId"].is_number_unsigned()) {
							r.sourceUniverseId = fr["sourceUniverseId"].get<uint64_t>();
						}
					}

					if (it.contains("mutualFriendsList") && it["mutualFriendsList"].is_array()) {
						for (const auto &m : it["mutualFriendsList"]) {
							if (m.is_string()) {
								try {
									r.mutuals.push_back(m.get<std::string>());
								} catch (...) {}
							}
						}
					}

					if (r.userId != 0) { requestUserIds.push_back(r.userId); }
					page.data.push_back(std::move(r));
				}
			}

			// Fetch names from User Profile API since Friends API no longer returns them
			if (!requestUserIds.empty()) {
				auto profiles = getUserProfiles(requestUserIds);
				for (auto &r : page.data) {
					auto it = profiles.find(r.userId);
					if (it != profiles.end()) {
						// Format is "combinedName|username"
						size_t sep = it->second.find('|');
						if (sep != std::string::npos) {
							r.displayName = it->second.substr(0, sep);
							r.username = it->second.substr(sep + 1);
						} else {
							r.displayName = it->second;
							r.username = it->second;
						}
					} else if (r.displayName.empty() && r.username.empty()) {
						// Fallback to user ID if profile fetch failed
						r.displayName = std::to_string(r.userId);
						r.username = std::to_string(r.userId);
					}
				}
			}
		} catch (const std::exception &e) {
			LOG_ERROR(std::string("Error parsing incoming friend requests: ") + e.what());
			// Return whatever was safely parsed so far
		}
		return page;
	}

	// ============================================================================
	// HBA-enabled friend request methods
	// ============================================================================

	/**
	 * Accept a friend request with HBA support
	 * @param targetUserId The user ID to accept
	 * @param config HBA authentication configuration
	 * @param outResponse Optional output for response text
	 * @return true if successful
	 */
	inline bool acceptFriendRequest(
		const std::string &targetUserId,
		const HBA::AuthConfig &config,
		std::string *outResponse = nullptr
	) {
		if (!canUseCookie(config.cookie)) {
			if (outResponse) { *outResponse = "Banned/warned cookie"; }
			return false;
		}
		std::string url = "https://friends.roblox.com/v1/users/" + targetUserId + "/accept-friend-request";

		auto resp = AuthenticatedHttp::postWithCSRF(url, config);

		if (outResponse) { *outResponse = resp.text; }
		return resp.status_code >= 200 && resp.status_code < 300;
	}

	/**
	 * Accept a friend request (legacy, no HBA)
	 */
	inline bool acceptFriendRequest(
		const std::string &targetUserId,
		const std::string &cookie,
		std::string *outResponse = nullptr
	) {
		HBA::AuthConfig config {.cookie = cookie, .hbaPrivateKey = "", .hbaEnabled = false};
		return acceptFriendRequest(targetUserId, config, outResponse);
	}

	inline uint64_t getUserIdFromUsername(const std::string &username) {
		nlohmann::json payload = {
			{"usernames",		  {username}},
			{"excludeBannedUsers", true	   }
		};

		auto resp = HttpClient::post("https://users.roblox.com/v1/usernames/users", {}, payload.dump());

		if (resp.status_code < 200 || resp.status_code >= 300) {
			LOG_ERROR("Username lookup failed: HTTP " + std::to_string(resp.status_code));
			return 0;
		}

		auto j = HttpClient::decode(resp);
		if (!j.contains("data") || j["data"].empty()) {
			LOG_ERROR("Username not found");
			return 0;
		}

		return j["data"][0].value("id", 0ULL);
	}

	/**
	 * Send a friend request with HBA support
	 * @param targetUserId The user ID to send request to
	 * @param config HBA authentication configuration
	 * @param outResponse Optional output for response text
	 * @return true if successful
	 */
	inline bool sendFriendRequest(
		const std::string &targetUserId,
		const HBA::AuthConfig &config,
		std::string *outResponse = nullptr
	) {
		if (!canUseCookie(config.cookie)) {
			if (outResponse) { *outResponse = "Banned/warned cookie"; }
			return false;
		}
		std::string url = "https://friends.roblox.com/v1/users/" + targetUserId + "/request-friendship";

		nlohmann::json body = {
			{"friendshipOriginSourceType", 0}
		};

		auto resp = AuthenticatedHttp::postWithCSRF(url, config, body.dump());

		if (outResponse) { *outResponse = resp.text; }

		if (resp.status_code < 200 || resp.status_code >= 300) {
			LOG_ERROR("Friend request failed HTTP " + std::to_string(resp.status_code) + ": " + resp.text);
			return false;
		}

		auto j = HttpClient::decode(resp);
		bool success = j.value("success", false);
		if (!success) { LOG_ERROR("Friend request API failure: " + resp.text); }
		return success;
	}

	/**
	 * Send a friend request (legacy, no HBA)
	 */
	inline bool sendFriendRequest(
		const std::string &targetUserId,
		const std::string &cookie,
		std::string *outResponse = nullptr
	) {
		HBA::AuthConfig config {.cookie = cookie, .hbaPrivateKey = "", .hbaEnabled = false};
		return sendFriendRequest(targetUserId, config, outResponse);
	}

	/**
	 * Unfriend a user with HBA support
	 * @param targetUserId The user ID to unfriend
	 * @param config HBA authentication configuration
	 * @param outResponse Optional output for response text
	 * @return true if successful
	 */
	inline bool
		unfriend(const std::string &targetUserId, const HBA::AuthConfig &config, std::string *outResponse = nullptr) {
		if (!canUseCookie(config.cookie)) {
			if (outResponse) { *outResponse = "Banned/warned cookie"; }
			return false;
		}
		std::string url = "https://friends.roblox.com/v1/users/" + targetUserId + "/unfriend";

		auto resp = AuthenticatedHttp::postWithCSRF(url, config);

		if (outResponse) { *outResponse = resp.text; }

		if (resp.status_code < 200 || resp.status_code >= 300) {
			LOG_ERROR("Unfriend failed HTTP " + std::to_string(resp.status_code) + ": " + resp.text);
			return false;
		}

		return true;
	}

	/**
	 * Unfriend a user (legacy, no HBA)
	 */
	inline bool
		unfriend(const std::string &targetUserId, const std::string &cookie, std::string *outResponse = nullptr) {
		HBA::AuthConfig config {.cookie = cookie, .hbaPrivateKey = "", .hbaEnabled = false};
		return unfriend(targetUserId, config, outResponse);
	}

	/**
	 * Follow a user with HBA support
	 * @param targetUserId The user ID to follow
	 * @param config HBA authentication configuration
	 * @param outResponse Optional output for response text
	 * @return true if successful
	 */
	inline bool
		followUser(const std::string &targetUserId, const HBA::AuthConfig &config, std::string *outResponse = nullptr) {
		if (!canUseCookie(config.cookie)) {
			if (outResponse) { *outResponse = "Banned/warned cookie"; }
			return false;
		}
		std::string url = "https://friends.roblox.com/v1/users/" + targetUserId + "/follow";

		auto resp = AuthenticatedHttp::postWithCSRF(url, config);

		if (outResponse) { *outResponse = resp.text; }
		return resp.status_code >= 200 && resp.status_code < 300;
	}

	/**
	 * Follow a user (legacy, no HBA)
	 */
	inline bool
		followUser(const std::string &targetUserId, const std::string &cookie, std::string *outResponse = nullptr) {
		HBA::AuthConfig config {.cookie = cookie, .hbaPrivateKey = "", .hbaEnabled = false};
		return followUser(targetUserId, config, outResponse);
	}

	/**
	 * Unfollow a user with HBA support
	 * @param targetUserId The user ID to unfollow
	 * @param config HBA authentication configuration
	 * @param outResponse Optional output for response text
	 * @return true if successful
	 */
	inline bool unfollowUser(
		const std::string &targetUserId,
		const HBA::AuthConfig &config,
		std::string *outResponse = nullptr
	) {
		if (!canUseCookie(config.cookie)) {
			if (outResponse) { *outResponse = "Banned/warned cookie"; }
			return false;
		}
		std::string url = "https://friends.roblox.com/v1/users/" + targetUserId + "/unfollow";

		auto resp = AuthenticatedHttp::postWithCSRF(url, config);

		if (outResponse) { *outResponse = resp.text; }
		return resp.status_code >= 200 && resp.status_code < 300;
	}

	/**
	 * Unfollow a user (legacy, no HBA)
	 */
	inline bool
		unfollowUser(const std::string &targetUserId, const std::string &cookie, std::string *outResponse = nullptr) {
		HBA::AuthConfig config {.cookie = cookie, .hbaPrivateKey = "", .hbaEnabled = false};
		return unfollowUser(targetUserId, config, outResponse);
	}

	/**
	 * Block a user with HBA support
	 * @param targetUserId The user ID to block
	 * @param config HBA authentication configuration
	 * @param outResponse Optional output for response text
	 * @return true if successful
	 */
	inline bool
		blockUser(const std::string &targetUserId, const HBA::AuthConfig &config, std::string *outResponse = nullptr) {
		if (!canUseCookie(config.cookie)) {
			if (outResponse) { *outResponse = "Banned/warned cookie"; }
			return false;
		}
		std::string url = "https://www.roblox.com/users/" + targetUserId + "/block";

		auto resp = AuthenticatedHttp::postWithCSRF(url, config);

		if (outResponse) { *outResponse = resp.text; }
		return resp.status_code >= 200 && resp.status_code < 300;
	}

	/**
	 * Block a user (legacy, no HBA)
	 */
	inline bool
		blockUser(const std::string &targetUserId, const std::string &cookie, std::string *outResponse = nullptr) {
		HBA::AuthConfig config {.cookie = cookie, .hbaPrivateKey = "", .hbaEnabled = false};
		return blockUser(targetUserId, config, outResponse);
	}
} // namespace Roblox
