#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace Console {
	void Log(const std::string &message);

	void RenderConsoleTab();

	std::vector<std::string> GetLogs(); // Added for potential external access
	std::string GetLatestLogMessageForStatus();
} // namespace Console
