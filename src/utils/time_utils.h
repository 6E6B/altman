#pragma once

#include <string>
#include <ctime>

std::string formatRelativeFuture(time_t timestamp);
std::string formatCountdown(time_t timestamp);
std::string formatAbsoluteLocal(time_t timestamp);
std::string formatAbsoluteFromIso(const std::string& isoUtcRaw);
std::string formatTimeOnlyLocal(time_t timestamp);
std::string formatRelativeToNow(time_t timestamp);
std::string formatAbsoluteWithRelativeLocal(time_t timestamp);
std::string formatAbsoluteWithRelativeFromIso(const std::string& isoUtcRaw);

time_t parseIsoTimestamp(const std::string& isoRaw);