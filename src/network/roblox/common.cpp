#include "common.h"

#include <imgui.h>
#include <random>
#include <cctype>
#include <charconv>

ImVec4 getStatusColor(std::string statusCode) {
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
        return ImVec4(1.0f, 0.8f, 0.0f, 1.0f);
    }
    if (statusCode == "Terminated") {
        return ImVec4(0.8f, 0.1f, 0.1f, 1.0f);
    }
    if (statusCode == "InvalidCookie") {
        return ImVec4(0.9f, 0.4f, 0.9f, 1.0f);
    }
    return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
}

std::string generateSessionId() {
    static const char* hex = "0123456789abcdef";
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

std::string presenceTypeToString(int type) {
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

bool parseUserSpecifier(std::string_view raw, UserSpecifier& out) {
    const std::string_view s = trim_view(raw);
    if (s.empty())
        return false;

    // Fast path: id=NUMBER (case-insensitive)
    if (s.size() > 3 &&
        (s[0] == 'i' || s[0] == 'I') &&
        (s[1] == 'd' || s[1] == 'D') &&
        s[2] == '=') {

        const std::string_view num = s.substr(3);
        if (num.empty())
            return false;

        uint64_t value{};
        const auto [ptr, ec] =
            std::from_chars(num.data(), num.data() + num.size(), value);

        if (ec != std::errc{} || ptr != num.data() + num.size())
            return false;

        out = {
            .isId = true,
            .id = value,
            .username = {}
        };
        return true;
    }

    // Username validation
    for (unsigned char ch : s) {
        if (!(std::isalnum(ch) || ch == '_'))
            return false;
    }

    out = {
        .isId = false,
        .id = 0,
        .username = std::string{s}
    };
    return true;
}