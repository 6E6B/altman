#pragma once

#include <string>
#include <functional>

using CookieCallback = std::function<void(const std::string&)>;

void LaunchWebview(const std::string &url,
				   const std::string &windowName = "Altman Webview",
				   const std::string &cookie = "",
				   const std::string &userId = "",
				   CookieCallback onCookieExtracted = nullptr);