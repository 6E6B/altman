#include "games_utils.h"
#include "core/time_utils.h"
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

using std::string;
using std::to_string;

string formatPrettyDate(const string &isoTimestampRaw) {
	time_t t = parseIsoTimestamp(isoTimestampRaw);
	if (t == 0) { return isoTimestampRaw; }
	string absStr = formatAbsoluteLocal(t);
	string relStr = formatRelativeToNow(t);
	if (!relStr.empty()) { return absStr + " (" + relStr + ")"; }
	return absStr;
}

string formatWithCommas(long long value) {
	bool isNegative = value < 0;
	unsigned long long absoluteValue = isNegative ? -value : value;
	string numberString = to_string(absoluteValue);
	for (int insertPosition = static_cast<int>(numberString.length()) - 3; insertPosition > 0; insertPosition -= 3) {
		numberString.insert(insertPosition, ",");
	}
	return isNegative ? "-" + numberString : numberString;
}
