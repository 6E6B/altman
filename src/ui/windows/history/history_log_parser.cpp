#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "ui/windows/history/history_log_parser.h"

namespace {
    constexpr std::string_view CHANNEL_TOKEN = "The channel is ";
    constexpr std::string_view VERSION_TOKEN = "\"version\":\"";
    constexpr std::string_view JOIN_TIME_TOKEN = "join_time:";
    constexpr std::string_view JOB_ID_TOKEN = "Joining game '";
    constexpr std::string_view PLACE_TOKEN = "place ";
    constexpr std::string_view UNIVERSE_TOKEN = "universeid:";
    constexpr std::string_view SERVER_TOKEN = "UDMUX Address = ";
    constexpr std::string_view PORT_TOKEN = ", Port = ";
    constexpr std::string_view USER_ID_TOKEN = "userId = ";
    constexpr std::string_view OUTPUT_TOKEN = "[FLog::Output]";

    constexpr size_t MAX_READ = 512 * 1024; // 512KB
    constexpr std::string_view DIGITS = "0123456789";
    constexpr std::string_view WHITESPACE = " \t\n\r";
    constexpr std::string_view NUMERIC_CHARS = "0123456789.";

    const std::regex GUID_REGEX(R"([0-9a-fA-F]{8}-(?:[0-9a-fA-F]{4}-){3}[0-9a-fA-F]{12})");

    [[nodiscard]] size_t findNextLine(std::string_view data, size_t pos) noexcept {
        const size_t newlinePos = data.find('\n', pos);
        return newlinePos == std::string_view::npos ? data.size() : newlinePos;
    }

    [[nodiscard]] std::string_view trimLine(std::string_view line) noexcept {
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        return line;
    }

    [[nodiscard]] bool isTimestampLine(std::string_view line) noexcept {
        return line.length() >= 20 && !line.empty() && std::isdigit(static_cast<unsigned char>(line[0]));
    }

    [[nodiscard]] std::string extractTimestamp(std::string_view line) {
        const size_t zPos = line.find('Z');
        if (zPos != std::string_view::npos && zPos < 30) {
            return std::string(line.substr(0, zPos + 1));
        }
        return {};
    }

    [[nodiscard]] std::string extractToken(std::string_view line, std::string_view token, std::string_view delimiters) {
        const size_t tokenPos = line.find(token);
        if (tokenPos == std::string_view::npos) {
            return {};
        }

        const size_t valueStart = tokenPos + token.length();
        const size_t valueEnd = line.find_first_of(delimiters, valueStart);
        const size_t length = (valueEnd == std::string_view::npos ? line.length() : valueEnd) - valueStart;

        return std::string(line.substr(valueStart, length));
    }

    [[nodiscard]] std::string extractQuotedValue(std::string_view line, std::string_view token) {
        const size_t tokenPos = line.find(token);
        if (tokenPos == std::string_view::npos) {
            return {};
        }

        const size_t valueStart = tokenPos + token.length();
        const size_t valueEnd = line.find('"', valueStart);

        if (valueEnd != std::string_view::npos) {
            return std::string(line.substr(valueStart, valueEnd - valueStart));
        }
        return {};
    }

    void processOutputLine(LogInfo &logInfo, std::string_view line) {
        if (line.find(OUTPUT_TOKEN) != std::string_view::npos) {
            logInfo.outputLines.emplace_back(line);
        }
    }

    void processChannel(LogInfo &logInfo, std::string_view line) {
        if (!logInfo.channel.empty()) {
            return;
        }
        logInfo.channel = extractToken(line, CHANNEL_TOKEN, WHITESPACE);
    }

    void processVersion(LogInfo &logInfo, std::string_view line) {
        if (!logInfo.version.empty()) {
            return;
        }
        logInfo.version = extractQuotedValue(line, VERSION_TOKEN);
    }

    void processJoinTime(LogInfo &logInfo, std::string_view line) {
        if (!logInfo.joinTime.empty()) {
            return;
        }
        logInfo.joinTime = extractToken(line, JOIN_TIME_TOKEN, WHITESPACE);
    }

    void processJobId(
        LogInfo &logInfo,
        std::string_view line,
        std::string_view timestamp,
        GameSession *&currentSession
    ) {
        const size_t tokenPos = line.find(JOB_ID_TOKEN);
        if (tokenPos == std::string_view::npos) {
            return;
        }

        const size_t valueStart = tokenPos + JOB_ID_TOKEN.length();
        const size_t valueEnd = line.find('\'', valueStart);

        if (valueEnd == std::string_view::npos) {
            return;
        }

        const std::string_view guidCandidate = line.substr(valueStart, valueEnd - valueStart);

        if (!std::regex_match(guidCandidate.begin(), guidCandidate.end(), GUID_REGEX)) {
            return;
        }

        GameSession newSession;
        newSession.timestamp = std::string(timestamp);
        newSession.jobId = std::string(guidCandidate);

        logInfo.sessions.push_back(std::move(newSession));
        currentSession = &logInfo.sessions.back();

        if (logInfo.jobId.empty()) {
            logInfo.jobId = currentSession->jobId;
        }
    }

    void processPlaceId(LogInfo &logInfo, std::string_view line, GameSession *currentSession) {
        if (currentSession == nullptr) {
            return;
        }

        std::string placeId = extractToken(line, PLACE_TOKEN, WHITESPACE);
        if (placeId.empty()) {
            return;
        }

        currentSession->placeId = placeId;

        if (logInfo.placeId.empty()) {
            logInfo.placeId = placeId;
        }
    }

    void processUniverseId(LogInfo &logInfo, std::string_view line, GameSession *currentSession) {
        if (currentSession == nullptr) {
            return;
        }

        std::string universeId = extractToken(line, UNIVERSE_TOKEN, WHITESPACE);
        if (universeId.empty()) {
            return;
        }

        currentSession->universeId = universeId;

        if (logInfo.universeId.empty()) {
            logInfo.universeId = universeId;
        }
    }

    void processServerInfo(LogInfo &logInfo, std::string_view line, GameSession *currentSession) {
        if (currentSession == nullptr) {
            return;
        }

        const size_t tokenPos = line.find(SERVER_TOKEN);
        if (tokenPos == std::string_view::npos) {
            return;
        }

        const size_t ipStart = tokenPos + SERVER_TOKEN.length();
        const size_t ipEnd = line.find(PORT_TOKEN, ipStart);

        if (ipEnd == std::string_view::npos) {
            return;
        }

        std::string ip(line.substr(ipStart, ipEnd - ipStart));

        const size_t portStart = ipEnd + PORT_TOKEN.length();
        std::string port = extractToken(line.substr(portStart), "", WHITESPACE);

        if (ip.empty() || port.empty()) {
            return;
        }

        currentSession->serverIp = std::move(ip);
        currentSession->serverPort = std::move(port);

        if (logInfo.serverIp.empty()) {
            logInfo.serverIp = currentSession->serverIp;
            logInfo.serverPort = currentSession->serverPort;
        }
    }

    void processUserId(LogInfo &logInfo, std::string_view line) {
        if (!logInfo.userId.empty()) {
            return;
        }
        logInfo.userId = extractToken(line, USER_ID_TOKEN, WHITESPACE);
    }

    void createBackwardCompatSession(LogInfo &logInfo) {
        if (!logInfo.sessions.empty()) {
            return;
        }

        if (logInfo.jobId.empty() && logInfo.placeId.empty()) {
            return;
        }

        GameSession session;
        session.timestamp = logInfo.timestamp;
        session.jobId = logInfo.jobId;
        session.placeId = logInfo.placeId;
        session.universeId = logInfo.universeId;
        session.serverIp = logInfo.serverIp;
        session.serverPort = logInfo.serverPort;

        logInfo.sessions.push_back(std::move(session));
    }

    void sortSessions(LogInfo &logInfo) {
        std::sort(logInfo.sessions.begin(), logInfo.sessions.end(), [](const GameSession &a, const GameSession &b) {
            return a.timestamp > b.timestamp;
        });
    }
} // namespace

std::filesystem::path GetLogsFolder() {
#ifdef _WIN32
    if (const wchar_t *localAppData = _wgetenv(L"LOCALAPPDATA")) {
        return std::filesystem::path(localAppData) / "Roblox" / "logs";
    }
#elif defined(__APPLE__)
    if (const char *home = std::getenv("HOME")) {
        return std::filesystem::path(home) / "Library" / "Logs" / "Roblox";
    }
#else
    if (const char *home = std::getenv("HOME")) {
        return std::filesystem::path(home) / "Roblox" / "logs";
    }
#endif

    return {};
}

void parseLogFile(LogInfo &logInfo) {
    if (logInfo.fileName.find("RobloxPlayerInstaller") != std::string::npos) {
        logInfo.isInstallerLog = true;
        return;
    }

    std::ifstream fileStream(logInfo.fullPath, std::ios::binary);
    if (!fileStream) {
        return;
    }

    std::string fileBuffer(MAX_READ, '\0');
    fileStream.read(fileBuffer.data(), MAX_READ);
    fileBuffer.resize(static_cast<size_t>(fileStream.gcount()));

    const std::string_view logData(fileBuffer);
    GameSession *currentSession = nullptr;
    std::string currentTimestamp;

    for (size_t pos = 0; pos < logData.size();) {
        const size_t lineEnd = findNextLine(logData, pos);
        std::string_view line = trimLine(logData.substr(pos, lineEnd - pos));

        if (isTimestampLine(line)) {
            std::string timestamp = extractTimestamp(line);
            if (!timestamp.empty()) {
                currentTimestamp = std::move(timestamp);

                if (logInfo.timestamp.empty()) {
                    logInfo.timestamp = currentTimestamp;
                }
            }
        }

        processOutputLine(logInfo, line);
        processChannel(logInfo, line);
        processVersion(logInfo, line);
        processJoinTime(logInfo, line);
        processJobId(logInfo, line, currentTimestamp, currentSession);
        processPlaceId(logInfo, line, currentSession);
        processUniverseId(logInfo, line, currentSession);
        processServerInfo(logInfo, line, currentSession);
        processUserId(logInfo, line);

        pos = lineEnd + 1;
    }

    createBackwardCompatSession(logInfo);
    sortSessions(logInfo);
}
