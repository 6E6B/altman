#pragma once

#include "../../components/data.h"
#include "crypto.h"
#include "logging.hpp"
#include <string_view>

namespace AccountFilters {

	inline bool IsBannedLikeStatus(std::string_view s) { return s == "Banned" || s == "Warned" || s == "Terminated"; }

	inline bool IsAccountUsable(const AccountData &a) { return !IsBannedLikeStatus(a.status); }

} // namespace AccountFilters

namespace AccountUtils {

	/**
	 * Generate a new HBA key pair for an account
	 * @param account The account to generate keys for
	 * @return true if keys were generated successfully
	 */
	inline bool generateHBAKeys(AccountData &account) {
		Crypto::ECKeyPair keyPair = Crypto::generateECKeyPair();
		if (!keyPair.isValid()) {
			LOG_ERROR("Failed to generate HBA keys for account: " + account.username);
			return false;
		}

		account.hbaPrivateKey = keyPair.privateKeyPEM;
		LOG_INFO("Generated HBA keys for account: " + account.username);
		return true;
	}

	/**
	 * Ensure an account has HBA keys, generating them if missing
	 * @param account The account to check/update
	 * @return true if account has valid HBA keys (existing or newly generated)
	 */
	inline bool ensureHBAKeys(AccountData &account) {
		if (!account.hbaPrivateKey.empty()) {
			return true; // Already has keys
		}

		return generateHBAKeys(account);
	}

	/**
	 * Check if an account has valid HBA configuration
	 * @param account The account to check
	 * @return true if HBA is enabled and keys are present
	 */
	inline bool hasValidHBA(const AccountData &account) { return account.hbaEnabled && !account.hbaPrivateKey.empty(); }

	/**
	 * Migrate all accounts to have HBA keys (for upgrades)
	 * @param accounts Vector of accounts to migrate
	 * @return Number of accounts that had keys generated
	 */
	inline int migrateAccountsToHBA(std::vector<AccountData> &accounts) {
		int migrated = 0;
		for (auto &account : accounts) {
			if (account.hbaPrivateKey.empty() && account.hbaEnabled) {
				if (generateHBAKeys(account)) { migrated++; }
			}
		}
		if (migrated > 0) { LOG_INFO("Migrated " + std::to_string(migrated) + " accounts to HBA"); }
		return migrated;
	}

} // namespace AccountUtils
