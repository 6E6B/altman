#pragma once

#include <string>
#include "data.h"
#include "ui/webview.hpp"

inline void LaunchWebview(const std::string &url, const AccountData &account) {
	std::string title = account.username;
	if (!account.userId.empty()) title += " - " + account.userId;
	LaunchWebview(url, title, account.cookie, account.userId, nullptr);
}

inline void LaunchWebview(const std::string &url, const AccountData &account, const std::string &windowName) {
	std::string title = windowName.empty() ? (account.username + (account.userId.empty() ? "" : (" - " + account.userId)))
										   : windowName;
	LaunchWebview(url, title, account.cookie, account.userId, nullptr);
}

inline void LaunchWebviewForLogin(const std::string &url, const std::string &windowName, CookieCallback onCookieExtracted) {
	LaunchWebview(url, windowName, "", "", onCookieExtracted);
}
