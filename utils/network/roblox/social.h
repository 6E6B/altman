#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

#include "http.hpp"
#include "core/logging.hpp"
#include "threading.h"

#include "../../components/components.h"

using namespace std;

namespace RobloxApi {
        static vector<FriendInfo> getFriends(const string &userId, const string &cookie) {
                LOG_INFO("Fetching friends list");

                HttpClient::Response resp = HttpClient::get(
                        "https://friends.roblox.com/v1/users/" + userId + "/friends",
                        {{"Cookie", ".ROBLOSECURITY=" + cookie}});

                if (resp.status_code != 200) {
                        LOG_ERROR("Failed to fetch friends: HTTP " + to_string(resp.status_code));
                        return {};
                }

                nlohmann::json j = HttpClient::decode(resp);
                vector<FriendInfo> friends;
                if (j.contains("data") && j["data"].is_array()) {
                        for (const auto &item: j["data"]) {
                                FriendInfo f;
                                f.id = item.value("id", 0ULL);
                                f.displayName = item.value("displayName", "");
                                f.username = item.value("name", "");
                                friends.push_back(f);
                        }
                }
                return friends;
        }

        static FriendInfo getUserInfo(const string &userId) {
                LOG_INFO("Fetching user info");
                HttpClient::Response resp = HttpClient::get(
                        "https://users.roblox.com/v1/users/" + userId,
                        {{"Accept", "application/json"}});

                if (resp.status_code != 200) {
                        LOG_ERROR("Failed to fetch user info: HTTP " + to_string(resp.status_code));
                        return FriendInfo{};
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
                string username;
                string displayName;
                string description;
                string createdIso;
                int followers = 0;
                int following = 0;
                int placeVisits = 0;
                string presence;
        };

        static FriendDetail getUserDetails(const string &userId,
                                           const string &cookie) {
                FriendDetail d;
                mutex m;
                condition_variable cv;
                int remaining = 3;

                auto signalDone = [&] {
                        lock_guard<mutex> lk(m);
                        if (--remaining == 0)
                                cv.notify_one();
                };

                Threading::newThread([&, userId] {
                        auto resp = HttpClient::get(
                                "https://users.roblox.com/v1/users/" + userId,
                                {{"Accept", "application/json"}}
                        );
                        if (resp.status_code == 200) {
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
                        auto resp = HttpClient::get(
                                "https://friends.roblox.com/v1/users/" + userId + "/followers/count",
                                {}
                        );
                        if (resp.status_code == 200) {
                                try {
                                        d.followers = nlohmann::json::parse(resp.text).value("count", 0);
                                } catch (const std::exception &e) {
                                        LOG_ERROR(std::string("Failed to parse followers count: ") + e.what());
                                }
                        }
                        signalDone();
                });

                Threading::newThread([&, userId] {
                        auto resp = HttpClient::get(
                                "https://friends.roblox.com/v1/users/" + userId + "/followings/count",
                                {}
                        );
                        if (resp.status_code == 200) {
                                try {
                                        d.following = nlohmann::json::parse(resp.text).value("count", 0);
                                } catch (const std::exception &e) {
                                        LOG_ERROR(std::string("Failed to parse following count: ") + e.what());
                                }
                        }
                        signalDone();
                });

                unique_lock<mutex> lk(m);
                cv.wait(lk, [&] {
                        return remaining == 0;
                });

                return d;
        }

        inline uint64_t getUserIdFromUsername(const string &username) {
                nlohmann::json payload = {
                        {"usernames", {username}},
                        {"excludeBannedUsers", true}
                };

                auto resp = HttpClient::post(
                        "https://users.roblox.com/v1/usernames/users",
                        {},
                        payload.dump());

                if (resp.status_code != 200) {
                        LOG_ERROR("Username lookup failed: HTTP " + to_string(resp.status_code));
                        return 0;
                }

                auto j = HttpClient::decode(resp);
                if (!j.contains("data") || j["data"].empty()) {
                        LOG_ERROR("Username not found");
                        return 0;
                }

                return j["data"][0].value("id", 0ULL);
        }

        inline bool sendFriendRequest(const string &targetUserId,
                                      const string &cookie,
                                      string *outResponse = nullptr) {
                string url = "https://friends.roblox.com/v1/users/" + targetUserId +
                             "/request-friendship";

                auto csrfResp = HttpClient::post(url, {{"Cookie", ".ROBLOSECURITY=" + cookie}});
                auto it = csrfResp.headers.find("x-csrf-token");
                if (it == csrfResp.headers.end()) {
                        if (outResponse) *outResponse = "Missing CSRF token";
                        cerr << "friend request: missing CSRF token\n";
                        return false;
                }

                nlohmann::json body = {
                        {"friendshipOriginSourceType", 0}
                };

                auto resp = HttpClient::post(
                        url,
                        {
                                {"Cookie", ".ROBLOSECURITY=" + cookie},
                                {"Origin", "https://www.roblox.com"},
                                {"Referer", "https://www.roblox.com/"},
                                {"X-CSRF-TOKEN", it->second}
                        },
                        body.dump());

                if (outResponse) *outResponse = resp.text;

                if (resp.status_code != 200) {
                        cerr << "friend request failed HTTP " << resp.status_code << ": " << resp.text << "\n";
                        return false;
                }

                auto j = HttpClient::decode(resp);
                bool success = j.value("success", false);
                if (success) {
                        cerr << "friend request success: " << resp.text << "\n";
                } else {
                        cerr << "friend request API failure: " << resp.text << "\n";
                }
                return success;
        }

        inline bool unfriend(const string &targetUserId,
                             const string &cookie,
                             string *outResponse = nullptr) {
                string url = "https://friends.roblox.com/v1/users/" + targetUserId +
                             "/unfriend";

                auto csrfResp = HttpClient::post(url, {{"Cookie", ".ROBLOSECURITY=" + cookie}});
                auto it = csrfResp.headers.find("x-csrf-token");
                if (it == csrfResp.headers.end()) {
                        if (outResponse) *outResponse = "Missing CSRF token";
                        return false;
                }

                auto resp = HttpClient::post(
                        url,
                        {
                                {"Cookie", ".ROBLOSECURITY=" + cookie},
                                {"Origin", "https://www.roblox.com"},
                                {"Referer", "https://www.roblox.com/"},
                                {"X-CSRF-TOKEN", it->second}
                        }
                );

                if (outResponse) *outResponse = resp.text;

                if (resp.status_code != 200) {
                        cerr << "unfriend failed HTTP " << resp.status_code << ": " << resp.text << "\n";
                        return false;
                }

                return true;
        }

        inline bool followUser(const string &targetUserId, const string &cookie, string *outResponse = nullptr) {
                string url = "https://friends.roblox.com/v1/users/" + targetUserId + "/follow";

                auto csrfResp = HttpClient::post(url, {{"Cookie", ".ROBLOSECURITY=" + cookie}});
                auto it = csrfResp.headers.find("x-csrf-token");
                if (it == csrfResp.headers.end()) {
                        if (outResponse) *outResponse = "Missing CSRF token";
                        return false;
                }

                auto resp = HttpClient::post(
                        url,
                        {
                                {"Cookie", ".ROBLOSECURITY=" + cookie},
                                {"Origin", "https://www.roblox.com"},
                                {"Referer", "https://www.roblox.com/"},
                                {"X-CSRF-TOKEN", it->second}
                        }
                );

                if (outResponse) *outResponse = resp.text;
                return resp.status_code == 200;
        }

        inline bool unfollowUser(const string &targetUserId, const string &cookie, string *outResponse = nullptr) {
                string url = "https://friends.roblox.com/v1/users/" + targetUserId + "/unfollow";

                auto csrfResp = HttpClient::post(url, {{"Cookie", ".ROBLOSECURITY=" + cookie}});
                auto it = csrfResp.headers.find("x-csrf-token");
                if (it == csrfResp.headers.end()) {
                        if (outResponse) *outResponse = "Missing CSRF token";
                        return false;
                }

                auto resp = HttpClient::post(
                        url,
                        {
                                {"Cookie", ".ROBLOSECURITY=" + cookie},
                                {"Origin", "https://www.roblox.com"},
                                {"Referer", "https://www.roblox.com/"},
                                {"X-CSRF-TOKEN", it->second}
                        }
                );

                if (outResponse) *outResponse = resp.text;
                return resp.status_code == 200;
        }

        inline bool blockUser(const string &targetUserId, const string &cookie,string *outResponse = nullptr) {
                string url = "https://www.roblox.com/users/" + targetUserId + "/block";

                auto csrfResp = HttpClient::post(url, {{"Cookie", ".ROBLOSECURITY=" + cookie}});
                auto it = csrfResp.headers.find("x-csrf-token");
                if (it == csrfResp.headers.end()) {
                        if (outResponse) *outResponse = "Missing CSRF token";
                        return false;
                }

                auto resp = HttpClient::post(
                        url,
                        {
                                {"Cookie", ".ROBLOSECURITY=" + cookie},
                                {"Origin", "https://www.roblox.com"},
                                {"Referer", "https://www.roblox.com/"},
                                {"X-CSRF-TOKEN", it->second}
                        }
                );

                if (outResponse) *outResponse = resp.text;
                return resp.status_code == 200;
        }
}

