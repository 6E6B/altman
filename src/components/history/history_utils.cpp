#define _CRT_SECURE_NO_WARNINGS
#include <iomanip>
#include <sstream>
#include <string>
#include <ctime>

#include "history_utils.h"
#include "log_types.h"
#include "core/time_utils.h"

using namespace std;

string friendlyTimestamp(const string &isoTimestamp) {
    if (isoTimestamp.empty()) return isoTimestamp;
    return formatAbsoluteFromIso(isoTimestamp);
}

string niceLabel(const LogInfo &logInfo) {
    if (logInfo.timestamp.size() >= 19) {
        time_t t = parseIsoTimestamp(logInfo.timestamp);
        if (t != static_cast<time_t>(-1) && t != 0) {
            // For list entries, show time-only as date headers already separate days
            return formatTimeOnlyLocal(t);
        }
    }
    return logInfo.fileName;
}
