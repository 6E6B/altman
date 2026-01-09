#pragma once

#include <string>
#include <vector>
#include <format>

namespace Console {

	enum class Level {
		Info,
		Warn,
		Error
	};

	void Log(Level level, const std::string& message);

	template <typename... Args>
	void Log(Level level, std::format_string<Args...> fmt, Args&&... args);

	void RenderConsoleTab();

	std::vector<std::string> GetLogs();
	std::string GetLatestLogMessageForStatus();

	template <typename... Args>
	inline void Log(Level level,
					std::format_string<Args...> fmt,
					Args&&... args) {
		Log(level, std::format(fmt, std::forward<Args>(args)...));
	}
}

#define LOG_INFO(...)  Console::Log(Console::Level::Info,  __VA_ARGS__)
#define LOG_WARN(...)  Console::Log(Console::Level::Warn,  __VA_ARGS__)
#define LOG_ERROR(...) Console::Log(Console::Level::Error, __VA_ARGS__)