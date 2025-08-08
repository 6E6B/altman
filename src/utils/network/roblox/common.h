#pragma once

#include <imgui.h>
#include <random>
#include <string>
#include <string_view>
#include <cstdint>
#include <cctype>


static ImVec4 getStatusColor(std::string statusCode) {
	if (statusCode == "Online") {
		return ImVec4(0.6f, 0.8f, 0.95f, 1.0f);
	}
	if (statusCode == "InGame") {
		return ImVec4(0.6f, 0.9f, 0.7f, 1.0f);
	}
	if (statusCode == "InStudio") {
		return ImVec4(1.0f, 0.85f, 0.7f, 1.0f);
	}
	if (statusCode == "Invisible") {
		return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
	}
	if (statusCode == "Banned") {
		return ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
	}
	if (statusCode == "Warned") {
		return ImVec4(1.0f, 0.8f, 0.3f, 1.0f);  // Orange/yellow for warnings
	}
	if (statusCode == "Terminated") {
		return ImVec4(0.8f, 0.1f, 0.1f, 1.0f);  // Darker red for terminated accounts
	}
	if (statusCode == "InvalidCookie") {
		return ImVec4(0.9f, 0.4f, 0.9f, 1.0f);  // Purple/magenta for invalid cookies
	}
	return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
}

static std::string generateSessionId() {
	static auto hex = "0123456789abcdef";
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<> dis(0, 15);

	std::string uuid(36, ' ');
	for (int i = 0; i < 36; i++) {
		switch (i) {
			case 8:
			case 13:
			case 18:
			case 23:
				uuid[i] = '-';
				break;
			case 14:
				uuid[i] = '4';
				break;
			case 19:
				uuid[i] = hex[(dis(gen) & 0x3) | 0x8];
				break;
			default:
				uuid[i] = hex[dis(gen)];
		}
	}
	return uuid;
}

static std::string presenceTypeToString(int type) {
	switch (type) {
		case 1:
			return "Online";
		case 2:
			return "InGame";
		case 3:
			return "InStudio";
		case 4:
			return "Invisible";
		default:
			return "Offline";
	}
}

struct UserSpecifier {
	bool isId = false;
	uint64_t id = 0;
	std::string username;
};

static inline std::string_view trim_view(std::string_view s) {
	size_t b = 0;
	size_t e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) ++b;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) --e;
	return s.substr(b, e - b);
}

static inline bool parseUserSpecifier(std::string_view raw, UserSpecifier &out) {
	std::string_view s = trim_view(raw);
	if (s.size() >= 3) {
		char c0 = s[0];
		char c1 = s[1];
		char c2 = s[2];
		if ((c0 == 'i' || c0 == 'I') && (c1 == 'd' || c1 == 'D') && c2 == '=') {
			std::string_view rest = s.substr(3);
			if (rest.empty()) return false;
			uint64_t val = 0;
			for (char ch : rest) {
				if (ch < '0' || ch > '9') return false;
				uint64_t d = static_cast<uint64_t>(ch - '0');
				uint64_t nv = val * 10 + d;
				if (nv < val) return false;
				val = nv;
			}
			out.isId = true;
			out.id = val;
			out.username.clear();
			return true;
		}
	}
	if (s.empty()) return false;
	for (char ch : s) {
		if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_'))
			return false;
	}
	out.isId = false;
	out.id = 0;
	out.username.assign(s.begin(), s.end());
	return true;
}
