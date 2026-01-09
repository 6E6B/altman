#include "account_utils.h"
#include "components/data.h"

namespace AccountFilters {

	bool IsAccountUsable(const AccountData& a) {
		return !IsBannedLikeStatus(a.status);
	}

}