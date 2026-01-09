#pragma once

#include <string_view>

struct AccountData;

namespace AccountFilters {

	inline bool IsBannedLikeStatus(std::string_view s) {
		return s == "Banned" || s == "Warned" || s == "Terminated";
	}

	bool IsAccountUsable(const AccountData& a);

}