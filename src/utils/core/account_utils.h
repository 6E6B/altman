#pragma once

#include "../../components/data.h"
#include <string_view>

namespace AccountFilters {

	inline bool IsBannedLikeStatus(std::string_view s) { return s == "Banned" || s == "Warned" || s == "Terminated"; }

	inline bool IsAccountUsable(const AccountData &a) { return !IsBannedLikeStatus(a.status); }

} // namespace AccountFilters
