#pragma once

#include <string>
#include "ui/windows/history/history_log_types.h"

std::string friendlyTimestamp(const std::string &isoTimestamp);

std::string niceLabel(const LogInfo &logInfo);
