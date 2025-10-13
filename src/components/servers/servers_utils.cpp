#include "servers_utils.h"
#include <algorithm>
#include <cctype>
#include <string>

std::string toLower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

bool containsCI(const std::string &haystack, const std::string &needle) {
	if (needle.empty()) { return true; }
	std::string h_lower = toLower(haystack);
	std::string n_lower = toLower(needle);
	return h_lower.find(n_lower) != std::string::npos;
}
