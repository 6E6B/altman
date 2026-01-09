#pragma once

#include <string>
#include <vector>

// Structure for a single game session within a log
struct GameSession {
    std::string timestamp;    // When this session started (ISO UTC)
    std::string jobId;        // Session-specific job ID
    std::string placeId;      // Place ID for this session
    std::string universeId;   // Universe ID for this session
    std::string serverIp;     // Server IP for this session
    std::string serverPort;   // Server port for this session
};

struct LogInfo {
	std::string fileName;
	std::string fullPath;
	std::string timestamp;    // First timestamp in log (ISO UTC)
	std::string version;      // Roblox client version
	std::string channel;      // Channel (production, etc.)
	std::string userId;       // User ID (same across sessions)
	bool isInstallerLog;      // Flag for installer logs that should be filtered
	
	// Multiple game sessions within a single log file
	std::vector<GameSession> sessions;
	
	// For backward compatibility
	std::string joinTime;     // raw string (deprecated)
	std::string jobId;        // First job ID found (deprecated)
	std::string placeId;      // First place ID found (deprecated)
	std::string universeId;   // First universe ID found (deprecated)
	std::string serverIp;     // First server IP found (deprecated)
	std::string serverPort;   // First server port found (deprecated)
	std::vector<std::string> outputLines; // captured [FLog::Output] lines
	
	LogInfo() : isInstallerLog(false) {} // Initialize to false by default
};
