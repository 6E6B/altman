#pragma once
#include <string>
#include <chrono>
#include <sstream>

inline std::string formatRelativeFuture(time_t timestamp) {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto future = system_clock::from_time_t(timestamp);
    auto diff = duration_cast<seconds>(future - now).count();
    if (diff <= 0)
        return "now";

    long long years = diff / 31556952LL; diff %= 31556952LL;
    long long months = diff / 2629746LL; diff %= 2629746LL;
    long long days = diff / 86400LL; diff %= 86400LL;
    long long hours = diff / 3600LL; diff %= 3600LL;
    long long minutes = diff / 60LL; diff %= 60LL;
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
    if (diff < 0)
        diff = 0;
    long long minutes = diff / 60;
    long long seconds = diff % 60;

    char buf[32];
    snprintf(buf, sizeof(buf), "%lld:%02lld", minutes, seconds);
    return std::string(buf);
}
