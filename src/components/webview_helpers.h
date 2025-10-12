#pragma once

#include "data.h"
#include "ui/webview.hpp"
#include <string>

inline void LaunchWebview(const std::string &url, const AccountData &account) {
	std::string title = account.username;
	if (!account.userId.empty()) { title += " - " + account.userId; }
	LaunchWebview(url, title, account.cookie, account.userId);
}

inline void LaunchWebview(const std::string &url, const AccountData &account, const std::string &windowName) {
	std::string title = windowName.empty()
						  ? (account.username + (account.userId.empty() ? "" : (" - " + account.userId)))
						  : windowName;
	LaunchWebview(url, title, account.cookie, account.userId);
}
