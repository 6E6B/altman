#pragma once

#include <string>
#include "log_types.h"

std::string friendlyTimestamp(const std::string &isoTimestamp);

std::string niceLabel(const LogInfo &logInfo);
