#pragma once

#include "../data.h"
#include <string>

void RenderAccountContextMenu(AccountData &account, const std::string &unique_context_menu_id);

void LaunchBrowserWithCookie(const AccountData &account);
