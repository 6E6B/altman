#pragma once

#include "ui/windows/history/history_log_types.h"
#include <string>

void parseLogFile(LogInfo &logInfo);

std::filesystem::path GetLogsFolder();
