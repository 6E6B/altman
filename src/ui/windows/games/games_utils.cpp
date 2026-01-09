#include "games_utils.h"
#include <string>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <cmath>
#include "utils/time_utils.h"

std::string formatPrettyDate(const std::string &isoTimestampRaw) {
    time_t t = parseIsoTimestamp(isoTimestampRaw);
    if (t == 0) return isoTimestampRaw;
    std::string absStr = formatAbsoluteLocal(t);
    std::string relStr = formatRelativeToNow(t);
    if (!relStr.empty()) return absStr + " (" + relStr + ")";
    return absStr;
}

std::string formatWithCommas(long long value) {
	bool isNegative = value < 0;
	unsigned long long absoluteValue = isNegative ? -value : value;
	std::string numberString = std::to_string(absoluteValue);
	for (int insertPosition = static_cast<int>(numberString.length()) - 3; insertPosition > 0; insertPosition -= 3)
		numberString.insert(insertPosition, ",");
	return isNegative ? "-" + numberString : numberString;
}
