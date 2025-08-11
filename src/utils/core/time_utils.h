#pragma once
#include <string>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <ctime>

inline std::string formatRelativeFuture(time_t timestamp) {
	using namespace std::chrono;
	auto now = system_clock::now();
	auto future = system_clock::from_time_t(timestamp);
	auto diff = duration_cast<seconds>(future - now).count();
	if (diff <= 0)
		return "now";

	long long years = diff / 31556952LL;
	diff %= 31556952LL;
	long long months = diff / 2629746LL;
	diff %= 2629746LL;
	long long days = diff / 86400LL;
	diff %= 86400LL;
	long long hours = diff / 3600LL;
	diff %= 3600LL;
	long long minutes = diff / 60LL;
	diff %= 60LL;
	long long seconds = diff;

	std::ostringstream ss;
	if (years > 0)
		ss << years << " year" << (years == 1 ? "" : "s");
	else if (months > 0)
		ss << months << " month" << (months == 1 ? "" : "s");
	else if (days > 0)
		ss << days << " day" << (days == 1 ? "" : "s");
	else if (hours > 0)
		ss << hours << " hour" << (hours == 1 ? "" : "s");
	else if (minutes > 0)
		ss << minutes << " minute" << (minutes == 1 ? "" : "s");
	else
		ss << seconds << " second" << (seconds == 1 ? "" : "s");
	return ss.str();
}

inline std::string formatCountdown(time_t timestamp) {
    using namespace std::chrono;

    auto now = system_clock::now();
    auto target = system_clock::from_time_t(timestamp);
    long long diff = duration_cast<seconds>(target - now).count();

    if (diff <= 0) return "now";

    long long days = diff / 86400;
    diff %= 86400;

    long long hours = diff / 3600;
    diff %= 3600;

    long long minutes = diff / 60;
    long long seconds = diff % 60;

    std::ostringstream ss;

    // if the difference is exactly n days w/ no remainder, return only "x day(s)"
    if (days > 0 && hours == 0 && minutes == 0 && seconds == 0) {
        ss << days << (days == 1 ? " day" : " days");
        return ss.str();
    }

    if (days > 0) {
        ss << days << (days == 1 ? " day " : " days ");
    }

    ss << std::setfill('0');

    // if hours > 0, include the hours part else, skip it (regardless of days)
    if (hours > 0) {
        // hours w/o & minutes w/ leading zeros
        ss << hours << ":" << std::setw(2) << minutes;
    } else {
        // minutes w/o leading zeros
        ss << minutes;
    }

    // seconds always w/ leading zeros since minutes should always exist
    ss << ":" << std::setw(2) << seconds;

    return ss.str();
}

inline time_t parseIsoTimestamp(const std::string &isoRaw) {
        std::string iso = isoRaw;
        if (auto dot = iso.find('.'); dot != std::string::npos)
                iso = iso.substr(0, dot) + 'Z';
        if (auto plus = iso.find('+'); plus != std::string::npos)
                iso = iso.substr(0, plus) + 'Z';
        std::tm tm{};
        std::istringstream ss(iso);
        ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        if (ss.fail())
                return 0;
#if defined(_WIN32)
        return _mkgmtime(&tm);
#else
        return timegm(&tm);
#endif
}

inline std::string formatAbsoluteLocal(time_t timestamp) {
    if (timestamp == 0) return "";

    std::tm localTm{};
#if defined(_WIN32)
    localtime_s(&localTm, &timestamp);
#else
    localtime_r(&timestamp, &localTm);
#endif

    static const char *WEEK_ABBR[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *MONTH_FULL[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };

    int year = localTm.tm_year + 1900;
    int day = localTm.tm_mday;
    int hour12 = localTm.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    bool isPm = localTm.tm_hour >= 12;

    std::ostringstream out;
    out << WEEK_ABBR[localTm.tm_wday] << ", "
        << day << ' ' << MONTH_FULL[localTm.tm_mon] << ' ' << year << ", "
        << hour12 << ':' << std::setfill('0') << std::setw(2) << localTm.tm_min
        << ':' << std::setfill('0') << std::setw(2) << localTm.tm_sec << ' '
        << (isPm ? "PM" : "AM");
    return out.str();
}

inline std::string formatAbsoluteFromIso(const std::string &isoUtcRaw) {
    time_t t = parseIsoTimestamp(isoUtcRaw);
    if (t == 0) return isoUtcRaw;
    return formatAbsoluteLocal(t);
}

inline std::string formatTimeOnlyLocal(time_t timestamp) {
    if (timestamp == 0) return "";
    std::tm localTm{};
#if defined(_WIN32)
    localtime_s(&localTm, &timestamp);
#else
    localtime_r(&timestamp, &localTm);
#endif
    int hour12 = localTm.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    bool isPm = localTm.tm_hour >= 12;
    std::ostringstream out;
    out << hour12 << ':' << std::setfill('0') << std::setw(2) << localTm.tm_min
        << ':' << std::setfill('0') << std::setw(2) << localTm.tm_sec
        << ' ' << (isPm ? "PM" : "AM");
    return out.str();
}

inline std::string formatRelativeToNow(time_t timestamp);

inline std::string formatAbsoluteWithRelativeLocal(time_t timestamp) {
    if (timestamp == 0) return "";
    std::string absStr = formatAbsoluteLocal(timestamp);
    std::string relStr = formatRelativeToNow(timestamp);
    if (relStr.empty()) return absStr;
    return absStr + " (" + relStr + ")"; 
}

inline std::string formatAbsoluteWithRelativeFromIso(const std::string &isoUtcRaw) {
    time_t t = parseIsoTimestamp(isoUtcRaw);
    if (t == 0) return isoUtcRaw;
    return formatAbsoluteWithRelativeLocal(t);
}

inline std::string formatRelativeToNow(time_t timestamp) {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto then = system_clock::from_time_t(timestamp);
    long long diffSeconds = duration_cast<seconds>(then - now).count();

    if (diffSeconds == 0) return "now";

    auto absSeconds = diffSeconds < 0 ? -diffSeconds : diffSeconds;

    if (diffSeconds <= 0) {
        if (absSeconds < 60) return "just now";
        long long years = absSeconds / 31556952LL; absSeconds %= 31556952LL;
        long long months = absSeconds / 2629746LL; absSeconds %= 2629746LL;
        long long days = absSeconds / 86400LL; absSeconds %= 86400LL;
        long long hours = absSeconds / 3600LL; absSeconds %= 3600LL;
        long long minutes = absSeconds / 60LL;

        std::ostringstream ss;
        if (years > 0) ss << years << " year" << (years == 1 ? "" : "s");
        else if (months > 0) ss << months << " month" << (months == 1 ? "" : "s");
        else if (days > 0) ss << days << " day" << (days == 1 ? "" : "s");
        else if (hours > 0) ss << hours << " hour" << (hours == 1 ? "" : "s");
        else ss << minutes << " minute" << (minutes == 1 ? "" : "s");
        ss << " ago";
        return ss.str();
    } else {
        long long years = absSeconds / 31556952LL; absSeconds %= 31556952LL;
        long long months = absSeconds / 2629746LL; absSeconds %= 2629746LL;
        long long days = absSeconds / 86400LL; absSeconds %= 86400LL;
        long long hours = absSeconds / 3600LL; absSeconds %= 3600LL;
        long long minutes = absSeconds / 60LL; long long seconds = absSeconds % 60LL;

        std::ostringstream ss;
        if (years > 0) ss << "in " << years << " year" << (years == 1 ? "" : "s");
        else if (months > 0) ss << "in " << months << " month" << (months == 1 ? "" : "s");
        else if (days > 0) ss << "in " << days << " day" << (days == 1 ? "" : "s");
        else if (hours > 0) ss << "in " << hours << " hour" << (hours == 1 ? "" : "s");
        else if (minutes > 0) ss << "in " << minutes << " minute" << (minutes == 1 ? "" : "s");
        else ss << "in " << seconds << " second" << (seconds == 1 ? "" : "s");
        return ss.str();
    }
}
