#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "log_parser.h"

namespace fs = std::filesystem;
using std::regex;
using std::regex_match;
using std::string;
using std::string_view;
using std::vector;

string logsFolder() {
	const char *localAppDataPath = std::getenv("LOCALAPPDATA");
	return localAppDataPath ? string(localAppDataPath) + "\\Roblox\\logs" : string {};
}

void parseLogFile(LogInfo &logInfo) {
	using namespace std::string_view_literals;

	// Skip installer logs - they contain "RobloxPlayerInstaller" in the filename
	if (logInfo.fileName.find("RobloxPlayerInstaller") != string::npos) {
		logInfo.isInstallerLog = true;
		return;
	}

	constexpr size_t kMaxRead = 512 * 1024; // halfMB
	std::ifstream fileInputStream(logInfo.fullPath, std::ios::binary);
	if (!fileInputStream) { return; }

	string fileBuffer(kMaxRead, '\0');
	fileInputStream.read(fileBuffer.data(), kMaxRead);
	fileBuffer.resize(static_cast<size_t>(fileInputStream.gcount()));
	string_view log_data_view(fileBuffer);

	auto nextLinePos = [&log_data_view](size_t currentPosition) -> size_t {
		size_t newlinePosition = log_data_view.find('\n', currentPosition);
		return newlinePosition == string_view::npos ? log_data_view.size() : newlinePosition;
	};

	// Track the current game session we're building
	GameSession *currentSession = nullptr;
	string currentTimestamp;

	for (size_t currentScanPosition = 0; currentScanPosition < log_data_view.size();) {
		size_t endOfLineIndex = nextLinePos(currentScanPosition);
		string_view currentLineView = log_data_view.substr(currentScanPosition, endOfLineIndex - currentScanPosition);

		if (!currentLineView.empty() && currentLineView.back() == '\r') { currentLineView.remove_suffix(1); }

		// Track timestamps for all lines to associate with sessions
		if (currentLineView.length() >= 20 && !currentLineView.empty() && std::isdigit(currentLineView[0])) {
			size_t timestampZIndex = currentLineView.find('Z');
			if (timestampZIndex != string_view::npos && timestampZIndex < 30) {
				// Found a timestamp line
				currentTimestamp = string(currentLineView.substr(0, timestampZIndex + 1));

				// Set the initial timestamp for the log if it's not set yet
				if (logInfo.timestamp.empty()) { logInfo.timestamp = currentTimestamp; }
			}
		}

		// Still collect output lines for compatibility with data saving/loading
		if (currentLineView.find("[FLog::Output]"sv) != string_view::npos) {
			logInfo.outputLines.emplace_back(currentLineView);
		}

		if (logInfo.channel.empty()) {
			constexpr auto channelToken = "The channel is "sv;
			auto channelTokenIndex = currentLineView.find(channelToken);
			if (channelTokenIndex != string_view::npos) {
				size_t valueStartIndex = channelTokenIndex + channelToken.length();
				auto valueEndIndex = currentLineView.find_first_of(" \t\n\r"sv, valueStartIndex);
				logInfo.channel = string(currentLineView.substr(
					valueStartIndex,
					(valueEndIndex == string_view::npos ? currentLineView.length() : valueEndIndex) - valueStartIndex
				));
			}
		}

		if (logInfo.version.empty()) {
			constexpr auto versionToken = "\"version\":\""sv;
			auto versionTokenIndex = currentLineView.find(versionToken);
			if (versionTokenIndex != string_view::npos) {
				size_t valueStartIndex = versionTokenIndex + versionToken.length();
				auto valueEndIndex = currentLineView.find('"', valueStartIndex);
				if (valueEndIndex != string_view::npos) {
					logInfo.version = string(currentLineView.substr(valueStartIndex, valueEndIndex - valueStartIndex));
				}
			}
		}

		if (logInfo.joinTime.empty()) {
			constexpr auto joinTimeToken = "join_time:"sv;
			auto joinTimeTokenIndex = currentLineView.find(joinTimeToken);
			if (joinTimeTokenIndex != string_view::npos) {
				size_t valueStartIndex = joinTimeTokenIndex + joinTimeToken.length();
				auto valueEndIndex = currentLineView.find_first_not_of("0123456789."sv, valueStartIndex);
				logInfo.joinTime = string(currentLineView.substr(
					valueStartIndex,
					(valueEndIndex == string_view::npos ? currentLineView.length() : valueEndIndex) - valueStartIndex
				));
			}
		}

		// Detect new game session by job ID
		static const regex s_guid_regex(R"([0-9a-fA-F]{8}-(?:[0-9a-fA-F]{4}-){3}[0-9a-fA-F]{12})");
		constexpr auto jobIdToken = "Joining game '"sv;
		auto jobIdTokenIndex = currentLineView.find(jobIdToken);
		if (jobIdTokenIndex != string_view::npos) {
			size_t valueStartIndex = jobIdTokenIndex + jobIdToken.length();
			auto valueEndIndex = currentLineView.find('\'', valueStartIndex); // Find closing quote
			if (valueEndIndex != string_view::npos) {
				string_view guidCandidateView
					= currentLineView.substr(valueStartIndex, valueEndIndex - valueStartIndex);

				string jobId;
				if (regex_match(guidCandidateView.begin(), guidCandidateView.end(), s_guid_regex)) {
					jobId = string(guidCandidateView);

					// Found a new game session
					GameSession newSession;
					newSession.timestamp = currentTimestamp;
					newSession.jobId = jobId;

					// Add this new session
					logInfo.sessions.push_back(newSession);
					currentSession = &logInfo.sessions.back();

					// For backward compatibility
					if (logInfo.jobId.empty()) { logInfo.jobId = jobId; }
				}
			}
		}

		// Look for place ID
		constexpr auto placeToken = "place "sv;
		auto placeTokenIndex = currentLineView.find(placeToken);
		if (placeTokenIndex != string_view::npos && currentSession != nullptr) {
			size_t valueStartIndex = placeTokenIndex + placeToken.length();
			auto valueEndIndex = currentLineView.find_first_not_of("0123456789"sv, valueStartIndex);
			string placeId = string(currentLineView.substr(
				valueStartIndex,
				(valueEndIndex == string_view::npos ? currentLineView.length() : valueEndIndex) - valueStartIndex
			));

			// If we have a current session, associate this place ID with it
			if (!placeId.empty()) {
				currentSession->placeId = placeId;

				// For backward compatibility
				if (logInfo.placeId.empty()) { logInfo.placeId = placeId; }
			}
		}

		// Look for universe ID
		constexpr auto universeToken = "universeid:"sv;
		auto universeTokenIndex = currentLineView.find(universeToken);
		if (universeTokenIndex != string_view::npos && currentSession != nullptr) {
			size_t valueStartIndex = universeTokenIndex + universeToken.length();
			auto valueEndIndex = currentLineView.find_first_not_of("0123456789"sv, valueStartIndex);
			string universeId = string(currentLineView.substr(
				valueStartIndex,
				(valueEndIndex == string_view::npos ? currentLineView.length() : valueEndIndex) - valueStartIndex
			));

			// If we have a current session, associate this universe ID with it
			if (!universeId.empty()) {
				currentSession->universeId = universeId;

				// For backward compatibility
				if (logInfo.universeId.empty()) { logInfo.universeId = universeId; }
			}
		}

		// Look for server information
		constexpr auto serverToken = "UDMUX Address = "sv;
		auto serverTokenIndex = currentLineView.find(serverToken);
		if (serverTokenIndex != string_view::npos && currentSession != nullptr) {
			size_t valueStartIndex = serverTokenIndex + serverToken.length();
			auto valueEndIndex = currentLineView.find(", Port = "sv, valueStartIndex);
			if (valueEndIndex != string_view::npos) {
				string ip = string(currentLineView.substr(valueStartIndex, valueEndIndex - valueStartIndex));

				constexpr auto portPrefixToken = ", Port = "sv;
				size_t portValueStartIndex = valueEndIndex + portPrefixToken.length();
				auto portValueEndIndex = currentLineView.find_first_not_of("0123456789"sv, portValueStartIndex);
				string port = string(currentLineView.substr(
					portValueStartIndex,
					(portValueEndIndex == string_view::npos ? currentLineView.length() : portValueEndIndex)
						- portValueStartIndex
				));

				// If we have a current session, associate this server info with it
				if (!ip.empty() && !port.empty()) {
					currentSession->serverIp = ip;
					currentSession->serverPort = port;

					// For backward compatibility
					if (logInfo.serverIp.empty()) {
						logInfo.serverIp = ip;
						logInfo.serverPort = port;
					}
				}
			}
		}

		if (logInfo.userId.empty()) {
			constexpr auto userIdToken = "userId = "sv;
			auto userIdTokenIndex = currentLineView.find(userIdToken);
			if (userIdTokenIndex != string_view::npos) {
				size_t valueStartIndex = userIdTokenIndex + userIdToken.length();
				auto valueEndIndex = currentLineView.find_first_not_of("0123456789"sv, valueStartIndex);
				logInfo.userId = string(currentLineView.substr(
					valueStartIndex,
					(valueEndIndex == string_view::npos ? currentLineView.length() : valueEndIndex) - valueStartIndex
				));
			}
		}

		currentScanPosition = endOfLineIndex + 1;
	}

	// If we didn't find any sessions but have jobId/placeId from old parsing logic,
	// create a synthetic session for backward compatibility
	if (logInfo.sessions.empty() && (!logInfo.jobId.empty() || !logInfo.placeId.empty())) {
		GameSession backwardCompatSession;
		backwardCompatSession.timestamp = logInfo.timestamp;
		backwardCompatSession.jobId = logInfo.jobId;
		backwardCompatSession.placeId = logInfo.placeId;
		backwardCompatSession.universeId = logInfo.universeId;
		backwardCompatSession.serverIp = logInfo.serverIp;
		backwardCompatSession.serverPort = logInfo.serverPort;
		logInfo.sessions.push_back(backwardCompatSession);
	}

	// Sort sessions in descending order of timestamps (newest first, oldest last)
	std::sort(logInfo.sessions.begin(), logInfo.sessions.end(), [](const GameSession &a, const GameSession &b) {
		return a.timestamp > b.timestamp;
	});
}
