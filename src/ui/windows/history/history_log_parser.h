#pragma once

#include <string>
#include "ui/windows/history/history_log_types.h"

void parseLogFile(LogInfo &logInfo);

std::filesystem::path GetLogsFolder();
