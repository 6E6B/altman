#pragma once

#include <string>
#include "components/data.h"

void RenderAccountContextMenu(AccountData &account, const std::string &unique_context_menu_id);

void LaunchBrowserWithCookie(const AccountData &account);
