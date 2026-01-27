#pragma once

#include <string>
#include "components/data.h"
#include "servers/servers.h"

void RenderAccountsTable(std::vector<AccountData> &, const char *, float);

bool RenderMainMenu();
