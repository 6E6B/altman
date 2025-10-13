#pragma once

#include "log_types.h"
#include <string>

std::string friendlyTimestamp(const std::string &isoTimestamp);

std::string niceLabel(const LogInfo &logInfo);
