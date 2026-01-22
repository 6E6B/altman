#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <optional>
#include <utility>

namespace SystemInfo {

	enum class Platform : uint8_t {
		Windows,
		macOS,
		Unknown
	};

	enum class CpuArchitecture : uint8_t {
		Arm64,
		x86_64,
		x86_64_Emulated,  // x64 on ARM via emulation (Windows) or Rosetta (macOS)
		Unknown
	};

	Platform GetCurrentPlatform() noexcept;
	std::string_view GetPlatformString() noexcept;

	CpuArchitecture GetCpuArchitecture();
	std::string_view GetArchitectureString();
	std::string GetHardwareArchitecture();

	bool IsRunningUnderEmulation();
#ifdef __APPLE__
	bool IsRunningUnderRosetta();

	struct SpawnOptions {
		std::map<std::string, std::string> env;
		std::optional<std::string> stdoutPath;
		std::optional<std::string> stderrPath;
		bool waitForCompletion = true;
	};

	bool ExecuteCommand(const std::string& command, std::string& output);

	bool SpawnProcessWithEnv(
		const char* program,
		const std::vector<const char*>& args,
		const SpawnOptions& opts);

	bool SpawnWithCustomHome(
		const char* program,
		const std::vector<const char*>& args,
		const std::string& customHome);
#endif

}