#pragma once

#include <string>
#include <functional>
#include <filesystem>

namespace IPAInstaller {
	using ProgressCallback = std::function<void(float progress, const std::string& message)>;
	using CompletionCallback = std::function<void(bool success, const std::string& message)>;

	bool ExecuteCommand(const std::string& command, std::string& output);
	std::string GetHardwareArchitecture();

	bool DownloadPackage(const std::string& appDataDir, const std::string& url,
						 const std::string& client, ProgressCallback progressCb);

	bool InstallPackage(const std::string& appDataDir, const std::string& client,
						ProgressCallback progressCb);

	bool RestructureAppBundle(const std::filesystem::path& appDir, ProgressCallback progressCb);

	bool Codesign(const std::filesystem::path& path, bool sign,
				  ProgressCallback progressCb);

	bool FixLibraryPaths(const std::filesystem::path& executablePath,
						 const std::filesystem::path& frameworksDir,
						 ProgressCallback progressCb);

	bool FixRpath(const std::filesystem::path& executablePath, ProgressCallback progressCb);

	bool Convert(const std::filesystem::path& path, const std::string& name,
				 ProgressCallback progressCb);

	bool ConvertPlist(const std::filesystem::path& path, ProgressCallback progressCb);
	bool ModifyPlist(const std::filesystem::path& plistPath, ProgressCallback progressCb);

	bool InstallIPA(const std::string& appDataDir, const std::string& client,
					const std::string& version, ProgressCallback progressCb);

	void InstallIPAAsync(const std::string& appDataDir, const std::string& client,
						 const std::string& version, ProgressCallback progressCb,
						 CompletionCallback completionCb);

}