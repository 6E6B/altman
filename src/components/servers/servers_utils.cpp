#include "servers_utils.h"
#include <algorithm>
#include <cctype>

using namespace std;

string toLower(string s) {
	transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
	return s;
}

bool containsCI(const string &haystack, const string &needle) {
	if (needle.empty()) { return true; }
	string h_lower = toLower(haystack);
	string n_lower = toLower(needle);
	return h_lower.find(n_lower) != string::npos;
}
