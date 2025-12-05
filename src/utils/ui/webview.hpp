#pragma once

#include <string>

void LaunchWebview(const std::string &url,
                   const std::string &windowName = "Altman Webview",
                   const std::string &cookie = "",
                   const std::string &userId = "");