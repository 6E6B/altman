#pragma once

#include <string>
#include <string_view>
#include <cstdint>

struct ImVec4;

ImVec4 getStatusColor(std::string statusCode);
std::string generateSessionId();
std::string presenceTypeToString(int type);

struct UserSpecifier {
	bool isId = false;
	uint64_t id = 0;
	std::string username;
};

constexpr std::string_view trim_view(std::string_view s) noexcept {
	auto is_space = [](unsigned char c) {
		return static_cast<bool>(std::isspace(c));
	};

	while (!s.empty() && is_space(s.front()))
		s.remove_prefix(1);

	while (!s.empty() && is_space(s.back()))
		s.remove_suffix(1);

	return s;
}

bool parseUserSpecifier(std::string_view raw, UserSpecifier& out);