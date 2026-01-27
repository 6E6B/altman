#pragma once

#include <functional>
#include <string>

#include "components/data.h"
#include "ui/webview/webview.h"

using CookieCallback = std::function<void(const std::string &)>;

void LaunchWebviewImpl(
    const std::string &url,
    const std::string &windowName = "Altman Webview",
    const std::string &cookie = "",
    const std::string &userId = "",
    CookieCallback onCookieExtracted = nullptr
);

void LaunchWebview(const std::string &url, const AccountData &account);

void LaunchWebview(const std::string &url, const AccountData &account, const std::string &windowName);

void LaunchWebviewForLogin(const std::string &url, const std::string &windowName, CookieCallback onCookieExtracted);
