#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

#include "http.hpp"
#include "core/logging.hpp"
#include "status.h"

using namespace std;

namespace RobloxApi {
        static string getPresence(
                const string &cookie,
                uint64_t userId) {
                LOG_INFO("Fetching user presence");
                nlohmann::json payload = {{"userIds", {userId}}};
                HttpClient::Response response = HttpClient::post(
                        "https://presence.roproxy.com/v1/presence/users",
                        {{"Cookie", ".ROBLOSECURITY=" + cookie}},
                        payload.dump());
                if (response.status_code != 200) {
                        LOG_ERROR("Presence lookup failed: HTTP " + to_string(response.status_code));

                        if (response.status_code == 403) {
                                return "Banned";
                        }

                        return "Offline";
                }

                OutputDebugStringA(("Raw response: " + response.text).c_str());
                LOG_INFO("Raw response body: " + response.text);

                auto json = HttpClient::decode(response);

                OutputDebugStringA(("Parsed JSON: " + json.dump()).c_str());
                LOG_INFO("Parsed JSON: " + json.dump());

                if (json.contains("userPresences") && json["userPresences"].is_array() && !json["userPresences"].empty()) {
                        const auto &jsonData = json["userPresences"][0];
                        int typeInt = jsonData.value("userPresenceType", 0);
                        LOG_INFO("Got user presence for " + to_string(userId));
                        return presenceTypeToString(typeInt);
                }
                return "Offline";
        }

        struct VoiceSettings {
                std::string status;
                time_t bannedUntil = 0;
        };

        static VoiceSettings getVoiceChatStatus(const string &cookie) {
                LOG_INFO("Fetching voice chat settings");
                auto resp = HttpClient::get(
                        "https://voice.roblox.com/v1/settings",
                        {{"Cookie", ".ROBLOSECURITY=" + cookie}}
                );

                if (resp.status_code != 200) {
                        LOG_INFO("Failed to fetch voice settings: HTTP " +
                                to_string(resp.status_code));
                        if (resp.status_code == 403)
                                return {"Banned", 0};
                        return {"Unknown", 0};
                }

                auto j = HttpClient::decode(resp);
                bool banned = j.value("isBanned", false);
                bool enabled = j.value("isVoiceEnabled", false);
                bool eligible = j.value("isUserEligible", false);
                bool opted = j.value("isUserOptIn", false);
                time_t bannedUntil = 0;
                if (j.contains("bannedUntil") && !j["bannedUntil"].is_null()) {
                        if (j["bannedUntil"].contains("Seconds"))
                                bannedUntil = j["bannedUntil"]["Seconds"].get<int64_t>();
                }

                if (banned)
                        return {"Banned", bannedUntil};
                if (enabled || opted)
                        return {"Enabled", 0};
                if (eligible)
                        return {"Disabled", 0};

                return {"Disabled", 0};
        }

        struct PresenceData {
                string presence;
                string lastLocation;
                uint64_t placeId = 0;
                string gameId;
        };

        static unordered_map<uint64_t, PresenceData>
        getPresences(const vector<uint64_t> &userIds,
                     const string &cookie) {
                nlohmann::json payload = {{"userIds", userIds}};

                auto resp = HttpClient::post(
                        "https://presence.roblox.com/v1/presence/users",
                        {{"Cookie", ".ROBLOSECURITY=" + cookie}},
                        payload.dump());

                if (resp.status_code != 200) {
                        LOG_ERROR("Batch presence failed: HTTP " + to_string(resp.status_code));
                        return {};
                }

                nlohmann::json j = HttpClient::decode(resp);
                unordered_map<uint64_t, PresenceData> out;

                if (j.contains("userPresences") && j["userPresences"].is_array()) {
                        for (auto &up: j["userPresences"]) {
                                PresenceData d;
                                d.presence = presenceTypeToString(up.value("userPresenceType", 0));
                                d.lastLocation = up.value("lastLocation", "");
                                if (up.contains("placeId") && up["placeId"].is_number_unsigned())
                                        d.placeId = up["placeId"].get<uint64_t>();
                                if (up.contains("gameId") && !up["gameId"].is_null())
                                        d.gameId = up["gameId"].get<string>();
                                if (up.contains("userId"))
                                        out[up["userId"].get<uint64_t>()] = move(d);
                        }
                }
                return out;
        }
}

