#include "friends_actions.h"
#include "core/status.h"
#include "network/roblox.h"
#include <algorithm>
#include <atomic>
#include <string>
#include <unordered_set>
#include <vector>

using std::atomic;
using std::find_if;
using std::move;
using std::string;
using std::unordered_set;
using std::vector;

static int presencePriority(const string &p) {
	if (p == "InGame") { return 0; }
	if (p == "InStudio") { return 1; }
	if (p == "Online") { return 2; }
	return 3;
}

namespace FriendsActions {
	void RefreshFullFriendsList(
		int accountId,
		const string &userId,
		const string &cookie,
		vector<FriendInfo> &outFriendsList,
		atomic<bool> &loadingFlag
	) {
		loadingFlag = true;
		LOG_INFO("Fetching friends list...");

		auto list = Roblox::getFriends(userId, cookie);

		vector<uint64_t> ids;
		ids.reserve(list.size());
		for (const auto &f : list) { ids.push_back(f.id); }

		for (auto &f : list) { f.presence = "Offline"; }

		LOG_INFO("Fetching friend presences...");

		for (size_t i = 0; i < ids.size(); i += 100) {
			size_t batchEnd = (std::min)(ids.size(), i + 100);
			vector<uint64_t> batch_ids(ids.begin() + i, ids.begin() + batchEnd);

			if (batch_ids.empty()) { continue; }

			auto presMap = Roblox::getPresences(batch_ids, cookie);

			for (const auto &[uid, pdata] : presMap) {
				auto it = find_if(list.begin(), list.end(), [&](const FriendInfo &f) { return f.id == uid; });
				if (it == list.end()) { continue; }

				it->presence = pdata.presence;
				it->lastLocation = pdata.lastLocation;
				it->placeId = pdata.placeId;
				it->jobId = pdata.jobId;
			}
		}

		sort(list.begin(), list.end(), [](const FriendInfo &a, const FriendInfo &b) {
			int pa = presencePriority(a.presence);
			int pb = presencePriority(b.presence);
			if (pa != pb) { return pa < pb; }

			// If both are “InGame”, push friends whose joins are *off*
			//      (lastLocation empty) *below* those whose joins are on.
			if (pa == 0) {
				// both “InGame”
				bool aJoinOff = a.lastLocation.empty();
				bool bJoinOff = b.lastLocation.empty();
				if (aJoinOff != bJoinOff) {
					return !aJoinOff; // joins‑ON first
				}
			}

			// ── fallback: alphabetical display name / username, then id ────────────
			const string &nameA_ref
				= (a.displayName.empty() || a.displayName == a.username) ? a.username : a.displayName;
			const string &nameB_ref
				= (b.displayName.empty() || b.displayName == b.username) ? b.username : b.displayName;

			if (nameA_ref.empty() && !nameB_ref.empty()) { return false; }
			if (!nameA_ref.empty() && nameB_ref.empty()) { return true; }
			if (nameA_ref.empty() && nameB_ref.empty()) { return a.id < b.id; }

			return nameA_ref < nameB_ref;
		});

		outFriendsList = move(list);

		// Build set of current friend IDs for quick membership checks
		unordered_set<uint64_t> newIds;
		for (const auto &f : outFriendsList) { newIds.insert(f.id); }

		// Detect newly lost friends by diffing previous cache vs current
		vector<FriendInfo> unfriended;
		auto itOld = g_accountFriends.find(accountId);
		if (itOld != g_accountFriends.end()) {
			for (const auto &oldF : itOld->second) {
				if (newIds.find(oldF.id) == newIds.end()) { unfriended.push_back(oldF); }
			}
		}

		// Update current friends cache
		g_accountFriends[accountId] = outFriendsList;

		// Merge newly detected unfriended, ensure container exists
		auto &stored = g_unfriendedFriends[accountId];
		std::unordered_set<uint64_t> seen;
		for (const auto &f : stored) { seen.insert(f.id); }
		for (const auto &f : unfriended) {
			if (seen.find(f.id) == seen.end()) {
				stored.push_back(f);
				seen.insert(f.id);
			}
		}

		// Validation: remove any unfriended that are now friends and dedupe
		if (!stored.empty()) {
			stored.erase(
				std::remove_if(
					stored.begin(),
					stored.end(),
					[&](const FriendInfo &fi) { return newIds.find(fi.id) != newIds.end(); }
				),
				stored.end()
			);
			std::vector<FriendInfo> dedup;
			dedup.reserve(stored.size());
			seen.clear();
			for (auto &u : stored) {
				if (seen.insert(u.id).second) { dedup.push_back(std::move(u)); }
			}
			stored.swap(dedup);
		}
		Data::SaveFriends();
		loadingFlag = false;
		LOG_INFO("Friends list updated.");
	}

	void FetchFriendDetails(
		const string &friendId,
		const string &cookie,
		Roblox::FriendDetail &outFriendDetail,
		atomic<bool> &loadingFlag
	) {
		loadingFlag = true;
		LOG_INFO("Fetching friend details...");
		outFriendDetail = Roblox::getUserDetails(friendId, cookie);
		loadingFlag = false;
		LOG_INFO("Friend details loaded.");
	}
} // namespace FriendsActions
