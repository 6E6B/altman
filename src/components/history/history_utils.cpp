#define _CRT_SECURE_NO_WARNINGS
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

#include "core/time_utils.h"
#include "history_utils.h"
#include "log_types.h"

std::string friendlyTimestamp(const std::string &isoTimestamp) {
	if (isoTimestamp.empty()) { return isoTimestamp; }
	return formatAbsoluteFromIso(isoTimestamp);
}

std::string niceLabel(const LogInfo &logInfo) {
	if (logInfo.timestamp.size() >= 19) {
		time_t t = parseIsoTimestamp(logInfo.timestamp);
		if (t != static_cast<time_t>(-1) && t != 0) {
			// For list entries, show time-only as date headers already separate days
			return formatTimeOnlyLocal(t);
		}
	}
	return logInfo.fileName;
}
