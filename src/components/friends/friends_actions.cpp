#include "friends_actions.h"
#include "network/roblox.h"
#include "core/status.h"
#include <algorithm>
#include <vector>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <ranges>
#include <execution>

namespace {
    constexpr int presencePriority(std::string_view presence) noexcept {
        if (presence == "InGame") return 0;
        if (presence == "InStudio") return 1;
        if (presence == "Online") return 2;
        return 3;
    }

    constexpr auto friendComparator = [](const FriendInfo& a, const FriendInfo& b) noexcept {
        const int pa = presencePriority(a.presence);
        const int pb = presencePriority(b.presence);
        
        if (pa != pb) return pa < pb;

        // Both "InGame": prioritize friends with joins enabled
        if (pa == 0) {
            const bool aJoinOff = a.lastLocation.empty();
            const bool bJoinOff = b.lastLocation.empty();
            if (aJoinOff != bJoinOff)
                return !aJoinOff;
        }

        // Fallback: alphabetical by display name or username
        const auto& nameA = (a.displayName.empty() || a.displayName == a.username) 
            ? a.username : a.displayName;
        const auto& nameB = (b.displayName.empty() || b.displayName == b.username) 
            ? b.username : b.displayName;

        if (nameA.empty() != nameB.empty()) return nameB.empty();
        if (nameA.empty()) return a.id < b.id;

        return nameA < nameB;
    };

    // Build index for O(1) lookups during presence updates
    std::unordered_map<uint64_t, FriendInfo*> buildFriendIndex(std::vector<FriendInfo>& friends) {
        std::unordered_map<uint64_t, FriendInfo*> index;
        index.reserve(friends.size());
        for (auto& f : friends) {
            index[f.id] = &f;
        }
        return index;
    }

    void updatePresences(
        std::unordered_map<uint64_t, FriendInfo*>& friendIndex,
        const std::vector<uint64_t>& ids,
        const std::string& cookie) {
        
        constexpr std::size_t BATCH_SIZE = 100;
        const std::size_t numBatches = (ids.size() + BATCH_SIZE - 1) / BATCH_SIZE;
        
        for (std::size_t batch = 0; batch < numBatches; ++batch) {
            const std::size_t start = batch * BATCH_SIZE;
            const std::size_t end = std::min(start + BATCH_SIZE, ids.size());
            const std::vector<uint64_t> batchIds(ids.begin() + start, ids.begin() + end);

            const auto presMap = Roblox::getPresences(batchIds, cookie);

            // O(1) lookup instead of O(n) find_if
            for (const auto& [uid, pdata] : presMap) {
                if (auto it = friendIndex.find(uid); it != friendIndex.end()) {
                    auto* friend_ptr = it->second;
                    friend_ptr->presence = pdata.presence;
                    friend_ptr->lastLocation = pdata.lastLocation;
                    friend_ptr->placeId = pdata.placeId;
                    friend_ptr->jobId = pdata.jobId;
                }
            }
        }
    }

    void deduplicateInPlace(std::vector<FriendInfo>& friends) {
        if (friends.empty()) return;
        
        std::unordered_set<uint64_t> seen;
        seen.reserve(friends.size());
        
        auto newEnd = std::remove_if(friends.begin(), friends.end(),
            [&seen](const FriendInfo& f) {
                return !seen.insert(f.id).second;
            });
        
        friends.erase(newEnd, friends.end());
    }
}

namespace FriendsActions {
    void RefreshFullFriendsList(
        int accountId,
        const std::string& userId,
        const std::string& cookie,
        std::vector<FriendInfo>& outFriendsList,
        std::atomic<bool>& loadingFlag) {
        
        loadingFlag = true;
        LOG_INFO("Fetching friends list...");

        auto list = Roblox::getFriends(userId, cookie);
        list.reserve(list.size());

        // Extract friend IDs (single pass)
        std::vector<uint64_t> ids;
        ids.reserve(list.size());
        for (const auto& f : list) {
            ids.push_back(f.id);
        }

        LOG_INFO("Fetching friend presences...");

        // Build index for O(1) presence updates
        auto friendIndex = buildFriendIndex(list);
        updatePresences(friendIndex, ids, cookie);

        std::ranges::sort(list, friendComparator);
        
        // Build current friend ID set
        std::unordered_set<uint64_t> newIds;
        newIds.reserve(list.size());
        for (const auto& f : list) {
            newIds.insert(f.id);
        }

        // Detect unfriended users (compute diff)
        std::vector<FriendInfo> unfriended;
        if (const auto itOld = g_accountFriends.find(accountId); itOld != g_accountFriends.end()) {
            unfriended.reserve(itOld->second.size() / 10); // Estimate 10% churn
            
            for (const auto& oldF : itOld->second) {
                if (!newIds.contains(oldF.id)) {
                    unfriended.push_back(oldF);
                }
            }
        }

        // Update cache before processing unfriended to ensure consistency
        g_accountFriends[accountId] = list;

        // Merge newly unfriended users efficiently
        if (!unfriended.empty()) {
            auto& stored = g_unfriendedFriends[accountId];
            
            // Build existing unfriended ID set
            std::unordered_set<uint64_t> existingIds;
            existingIds.reserve(stored.size());
            for (const auto& f : stored) {
                existingIds.insert(f.id);
            }
            
            // Add new unfriended that aren't already tracked
            for (auto&& f : unfriended) {
                if (existingIds.insert(f.id).second) {
                    stored.push_back(std::move(f));
                }
            }
            
            // Clean up: remove anyone who's a friend again
            std::erase_if(stored, [&newIds](const FriendInfo& fi) {
                return newIds.contains(fi.id);
            });
            
            // Deduplicate in single pass
            deduplicateInPlace(stored);
        }

        outFriendsList = std::move(list);
        
        Data::SaveFriends();
        loadingFlag = false;
        LOG_INFO("Friends list updated.");
    }

    void FetchFriendDetails(
        const std::string& friendId,
        const std::string& cookie,
        Roblox::FriendDetail& outFriendDetail,
        std::atomic<bool>& loadingFlag) {
        
        loadingFlag = true;
        LOG_INFO("Fetching friend details...");
        outFriendDetail = Roblox::getUserDetails(friendId, cookie);
        loadingFlag = false;
        LOG_INFO("Friend details loaded.");
    }
}