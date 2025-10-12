#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "../data.h"
#include "network/roblox.h"

namespace FriendsActions {
	void RefreshFullFriendsList(
		int accountId,
		const std::string &userId,
		const std::string &cookie,
		std::vector<FriendInfo> &outFriendsList,
		std::atomic<bool> &loadingFlag
	);

	void FetchFriendDetails(
		const std::string &friendId,
		const std::string &cookie,
		Roblox::FriendDetail &outFriendDetail,
		std::atomic<bool> &loadingFlag
	);
} // namespace FriendsActions
