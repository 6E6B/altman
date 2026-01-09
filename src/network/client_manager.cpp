#include "client_manager.h"

#include <filesystem>
#include <fstream>
#include <format>
#include <thread>
#include <array>
#include <regex>
#include <algorithm>
#include <cctype>
#include <sys/sysctl.h>

#include "console/console.h"
#include "http.h"
#include "ipa_installer.h"
#include "multi_instance.h"
#include "client_update_checker.h"

namespace ClientManager {

std::string GetHardwareArchitecture() {
    int isArm = 0;
    size_t size = sizeof(isArm);

    if (sysctlbyname("hw.optional.arm64", &isArm, &size, nullptr, 0) == 0 && isArm) {
        return "aarch64";
    }
    return "x86_64";
}

bool IsRunningUnderRosetta() {
    int translated = 0;
    size_t size = sizeof(translated);

    if (sysctlbyname("sysctl.proc_translated", &translated, &size, nullptr, 0) == 0) {
        return translated == 1;
    }
    return false;
}

std::string GetEffectiveArchitecture() {
    if (IsRunningUnderRosetta())
        return "x86_64 (Rosetta)";

    return GetHardwareArchitecture();
}

Architecture DetectArchitecture() {
    if (IsRunningUnderRosetta())
        return Architecture::X86_64_Rosetta;

    if (GetHardwareArchitecture() == "arm64")
        return Architecture::Arm64;

    return Architecture::X86_64;
}

bool ExecuteCommand(const std::string& command, std::string& output) {
    std::array<char, 128> buffer;
    output.clear();

    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
    if (!pipe) {
        return false;
    }

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        output += buffer.data();
    }

    const int exitCode = pclose(pipe);
    return WEXITSTATUS(exitCode) == 0;
}

std::string GetLatestRobloxVersion() {
    const std::string url = "https://clientsettings.roblox.com/v2/client-version/MacPlayer";

    const auto response = HttpClient::get(
        url,
        {{"User-Agent", "RobloxAccountManager/1.0"}},
        {}, true, 10
    );

    if (response.status_code != 200) {
        LOG_ERROR("Failed to fetch version: HTTP {}", response.status_code);
        return "";
    }

    const std::string versionKey = R"("clientVersionUpload":")";
    const size_t pos = response.text.find(versionKey);
    if (pos == std::string::npos) {
        LOG_ERROR("Failed to parse version from response");
        return "";
    }

    const size_t start = pos + versionKey.length();
    const size_t end = response.text.find('"', start);
    if (end == std::string::npos) {
        LOG_ERROR("Failed to parse version from response");
        return "";
    }

    return response.text.substr(start, end - start);
}

MacsploitVersion GetMacsploitVersion() {
    const std::string url = "https://git.raptor.fun/main/version.json";

    const auto response = HttpClient::get(
        url,
        {{"User-Agent", "RobloxAccountManager/1.0"}},
        {}, true, 10
    );

    if (response.status_code != 200) {
        LOG_ERROR("Failed to fetch Macsploit version: HTTP {}", response.status_code);
        throw std::runtime_error(std::format("HTTP {}", response.status_code));
    }

    try {
        nlohmann::json j = nlohmann::json::parse(response.text);
        MacsploitVersion version;
        version.clientVersionUpload = j["clientVersionUpload"].get<std::string>();
        version.appVersion = j["appVersion"].get<std::string>();
        version.clientVersion = j["clientVersion"].get<std::string>();
        version.relVersion = j["relVersion"].get<std::string>();
        version.channel = j["channel"].get<std::string>();
        version.changelog = j["changelog"].get<std::string>();
        return version;
    } catch (const nlohmann::json::exception& e) {
        LOG_ERROR("Failed to parse Macsploit version: {}", e.what());
        throw std::runtime_error(e.what());
    }
}

HydrogenVersion GetHydrogenVersion() {
    const std::string url = "https://hydrogen.lat/updates.json";

    const auto response = HttpClient::get(
        url,
        {{"User-Agent", "RobloxAccountManager/1.0"}},
        {}, true, 10
    );

    if (response.status_code != 200) {
        LOG_ERROR("Failed to fetch Hydrogen version: HTTP {}", response.status_code);
        throw std::runtime_error(std::format("HTTP {}", response.status_code));
    }

    try {
        nlohmann::json j = nlohmann::json::parse(response.text);
        HydrogenVersion version;

        version.global.globallogs = j["global"]["globallogs"].get<std::string>();

        auto parsePlatform = [](const nlohmann::json& platform) -> PlatformVersion {
            PlatformVersion pv;
            if (platform.contains("product") && !platform["product"].is_null())
                pv.product = platform["product"].get<std::string>();
            if (platform.contains("exploit_version") && !platform["exploit_version"].is_null())
                pv.exploit_version = platform["exploit_version"].get<std::string>();
            if (platform.contains("roblox_version") && !platform["roblox_version"].is_null())
                pv.roblox_version = platform["roblox_version"].get<std::string>();
            if (platform.contains("changelog") && !platform["changelog"].is_null())
                pv.changelog = platform["changelog"].get<std::string>();
            return pv;
        };

        version.windows = parsePlatform(j["windows"]);
        version.macos = parsePlatform(j["macos"]);
        version.ios = parsePlatform(j["ios"]);
        version.android = parsePlatform(j["android"]);

        return version;
    } catch (const nlohmann::json::exception& e) {
        LOG_ERROR("Failed to parse Hydrogen version: {}", e.what());
        throw std::runtime_error(e.what());
    }
}

std::string GetDeltaVersion() {
    const std::string url = "https://gloopup.net/Delta/ios/";

    const auto response = HttpClient::get(
        url,
        {{"User-Agent", "RobloxAccountManager/1.0"}},
        {}, true, 10
    );

    if (response.status_code != 200) {
        LOG_ERROR("Failed to fetch Delta version: HTTP {}", response.status_code);
        throw std::runtime_error(std::format("HTTP {}", response.status_code));
    }

    const std::string prefix = "https://cdn.gloopup.net/file/Delta-";
    const size_t pos = response.text.find(prefix);

    if (pos == std::string::npos) {
        LOG_ERROR("Delta server has returned an invalid response.");
        throw std::runtime_error("Delta server has returned an invalid response.");
    }

    const size_t start = pos + prefix.length();
    const size_t end = response.text.find(".ipa", start);

    if (end == std::string::npos) {
        LOG_ERROR("Delta server has returned an invalid response.");
        throw std::runtime_error("Delta server has returned an invalid response.");
    }

    return response.text.substr(start, end - start);
}

bool DownloadRoblox(const std::string& version, const std::string& outputPath,
                    ProgressCallback progressCb) {
    const std::string arch = GetHardwareArchitecture();
    const std::string url = (arch == "aarch64")
        ? std::format("https://setup.rbxcdn.com/mac/arm64/{}-RobloxPlayer.zip", version)
        : std::format("https://setup.rbxcdn.com/mac/{}-RobloxPlayer.zip", version);

    LOG_INFO("Downloading Roblox {} from {}", version, url);

    auto progress_adapter = [progressCb](size_t downloaded, size_t total) {
        if (progressCb && total > 0) {
            const float percent = static_cast<float>(downloaded) / static_cast<float>(total);
            progressCb(percent, std::format("Downloaded {} / {} bytes", downloaded, total));
        }
    };

    return HttpClient::download(
        url,
        outputPath,
        {{"User-Agent", "RobloxAccountManager/1.0"}},
        progress_adapter
    );
}

bool ExtractRoblox(const std::string& zipPath, const std::string& extractTo,
                   ProgressCallback progressCb) {
    if (progressCb) {
        progressCb(0.0f, "Extracting Roblox...");
    }

    std::error_code ec;
    std::filesystem::create_directories(extractTo, ec);
    if (ec) {
        LOG_ERROR("Failed to create directory: {}", ec.message());
        return false;
    }

    const std::string command = std::format("unzip -o -q \"{}\" -d \"{}\"", zipPath, extractTo);
    std::string output;

    if (!ExecuteCommand(command, output)) {
        LOG_ERROR("Failed to extract: {}", output);
        return false;
    }

    if (progressCb) {
        progressCb(1.0f, "Extraction complete");
    }

    return true;
}

bool CleanupRobloxApp(const std::string& clientsDir, ProgressCallback progressCb) {
    if (progressCb) {
        progressCb(0.0f, "Cleaning up...");
    }

    const std::filesystem::path robloxPlayerPath = std::filesystem::path(clientsDir) / "RobloxPlayer.app";
    const std::filesystem::path executableDir = robloxPlayerPath / "Contents" / "MacOS";

    std::error_code ec;
    std::filesystem::remove_all(executableDir / "Roblox.app", ec);
    std::filesystem::remove_all(executableDir / "RobloxPlayerInstaller.app", ec);

    if (progressCb) {
        progressCb(1.0f, "Cleanup complete");
    }

    return true;
}

bool DownloadInsertDylib(const std::string& outputPath, ProgressCallback progressCb) {
    const std::string url = "https://github.com/DollarNoob/Macsploit-Mirror/raw/main/insert_dylib";

    LOG_INFO("Downloading insert_dylib");

    auto progress_adapter = [progressCb](size_t downloaded, size_t total) {
        if (progressCb && total > 0) {
            const float percent = static_cast<float>(downloaded) / static_cast<float>(total);
            progressCb(percent, std::format("Downloaded {} / {} bytes", downloaded, total));
        }
    };

    if (!HttpClient::download(url, outputPath, {}, progress_adapter)) {
        return false;
    }

    const std::string command = std::format("chmod +x \"{}\"", outputPath);
    std::string output;

    return ExecuteCommand(command, output);
}

bool DownloadDylib(const std::string& clientName, const std::string& outputPath,
                   ProgressCallback progressCb) {
    const std::string arch = GetHardwareArchitecture();
    const std::string appDataDir = MultiInstance::getAppDataDirectory();

    if (clientName == "MacSploit") {
        const std::string url = (arch == "aarch64")
            ? "https://git.raptor.fun/arm/macsploit.dylib"
            : "https://git.raptor.fun/main/macsploit.dylib";

        LOG_INFO("Downloading dylib for MacSploit from {}", url);

        auto progress_adapter = [progressCb](size_t downloaded, size_t total) {
            if (progressCb && total > 0) {
                const float percent = static_cast<float>(downloaded) / static_cast<float>(total);
                progressCb(percent, std::format("Downloaded {} / {} bytes", downloaded, total));
            }
        };

        return HttpClient::download(url, outputPath, {}, progress_adapter);
    }
    else if (clientName == "Hydrogen" || clientName == "Ronix") {
        const std::string installUrl = (clientName == "Hydrogen")
            ? "https://www.hydrogen.lat/install"
            : "https://www.ronixmac.lol/install";

        LOG_INFO("Fetching {} install script", clientName);

        const auto response = HttpClient::get(installUrl, {}, {}, true, 30);

        if (response.status_code != 200) {
            LOG_ERROR("Failed to fetch install script: HTTP {}", response.status_code);
            return false;
        }

        const std::string regexPattern = std::format(
            R"REGEX({}_M_URL="(https://\w+\.ufs\.sh/f/\w+)")REGEX",
            clientName == "Hydrogen" ? "HYDROGEN" : "RONIX"
        );

        std::regex urlRegex(regexPattern);
        std::smatch matches;

        if (!std::regex_search(response.text, matches, urlRegex) || matches.size() < 2) {
            LOG_ERROR("Failed to parse {}.zip download URL from install script", clientName);
            return false;
        }

        const std::string zipUrl = matches[1].str();
        LOG_INFO("Found {}.zip URL: {}", clientName, zipUrl);

        const std::filesystem::path clientsDir = std::filesystem::path(appDataDir) / "clients";
        const std::filesystem::path zipPath = clientsDir / (clientName + ".zip");
        const std::filesystem::path appPath = clientsDir / (clientName + ".app");

        LOG_INFO("Downloading {}.zip", clientName);

        auto zipProgress = [progressCb](size_t downloaded, size_t total) {
            if (progressCb && total > 0) {
                const float percent = static_cast<float>(downloaded) / static_cast<float>(total) * 0.5f;
                progressCb(percent, std::format("Downloading zip: {} / {} bytes", downloaded, total));
            }
        };

        if (!HttpClient::download(zipUrl, zipPath.string(), {}, zipProgress)) {
            LOG_ERROR("Failed to download {}.zip", clientName);
            return false;
        }

        if (progressCb) {
            progressCb(0.5f, "Extracting zip...");
        }

        const std::string unzipCmd = std::format(
            "unzip -o -q \"{}\" -d \"{}\"",
            zipPath.string(),
            clientsDir.string()
        );

        std::string unzipOutput;
        if (!ExecuteCommand(unzipCmd, unzipOutput)) {
            LOG_ERROR("Failed to unzip {}: {}", clientName, unzipOutput);
            std::filesystem::remove(zipPath);
            return false;
        }

        if (progressCb) {
            progressCb(0.7f, "Verifying extraction...");
        }

        if (!std::filesystem::exists(appPath)) {
            LOG_ERROR("Failed to extract {}.app, application does not exist", clientName);
            std::filesystem::remove(zipPath);
            return false;
        }

        const std::filesystem::path executableDir = appPath / "Contents" / "MacOS";
        const std::string dylibFilename = (arch == "aarch64")
            ? (clientName + "-arm.dylib")
            : (clientName + "-intel.dylib");

        const std::filesystem::path sourceDylibPath = executableDir / dylibFilename;

        if (!std::filesystem::exists(sourceDylibPath)) {
            LOG_ERROR("Dylib not found at expected path: {}", sourceDylibPath.string());
            std::filesystem::remove(zipPath);
            std::filesystem::remove_all(appPath);
            return false;
        }

        if (progressCb) {
            progressCb(0.9f, "Moving dylib...");
        }

        std::error_code ec;
        std::filesystem::rename(sourceDylibPath, outputPath, ec);
        if (ec) {
            LOG_ERROR("Failed to move dylib: {}", ec.message());
            std::filesystem::remove(zipPath);
            std::filesystem::remove_all(appPath);
            return false;
        }

        std::filesystem::remove(zipPath, ec);
        std::filesystem::remove_all(appPath, ec);

        if (progressCb) {
            progressCb(1.0f, "Dylib download complete");
        }

        LOG_INFO("Successfully downloaded {} dylib", clientName);
        return true;
    }
    else if (clientName == "Delta") {
        LOG_ERROR("Delta Client Not Yet Available");
        return false;
    }
    else {
        LOG_ERROR("Unknown client: {}", clientName);
        return false;
    }
}

bool InsertDylib(const std::string& insertDylibPath, const std::string& dylibPath,
                 const std::string& binaryPath, ProgressCallback progressCb) {
    if (progressCb) {
        progressCb(0.0f, "Inserting dylib...");
    }

    const std::filesystem::path dylibPathObj(dylibPath);
    const std::string dylibFilename = dylibPathObj.filename().string();

    const std::string relativeDylibPath = "@executable_path/" + dylibFilename;

    const std::string command = std::format(
        "\"{}\" \"{}\" \"{}\" \"{}\" --overwrite --strip-codesig --all-yes",
        insertDylibPath, relativeDylibPath, binaryPath, binaryPath
    );

    std::string output;

    if (!ExecuteCommand(command, output)) {
        LOG_ERROR("Failed to insert dylib: {}", output);
        return false;
    }

    if (output.find("Added LC_LOAD_DYLIB") == std::string::npos) {
        LOG_ERROR("Unexpected insert_dylib output: {}", output);
        return false;
    }

    if (progressCb) {
        progressCb(1.0f, "Dylib inserted");
    }

    return true;
}

bool CodeSign(const std::string& appPath, bool remove, ProgressCallback progressCb) {
    if (progressCb) {
        progressCb(0.0f, remove ? "Removing signature..." : "Signing app...");
    }

    const std::string command = remove
        ? std::format("codesign --remove-signature \"{}\"", appPath)
        : std::format("codesign --force -s - \"{}\"", appPath);

    std::string output;

    if (!ExecuteCommand(command, output)) {
        LOG_ERROR("Codesign failed: {}", output);
        return false;
    }

    if (progressCb) {
        progressCb(1.0f, remove ? "Signature removed" : "App signed");
    }

    return true;
}

void InstallClientAsync(const std::string& clientName,
                        ProgressCallback progressCb,
                        CompletionCallback completionCb) {
	std::thread([clientName, progressCb, completionCb]() {
		const std::string appDataDir = MultiInstance::getAppDataDirectory();
		if (appDataDir.empty()) {
			if (completionCb) completionCb(false, "Failed to get app data directory");
			return;
		}

		const std::filesystem::path clientsDir = std::filesystem::path(appDataDir) / "clients";
		const std::filesystem::path finalAppPath = clientsDir / std::format("{}.app", clientName);

		if (std::filesystem::exists(finalAppPath)) {
			if (completionCb) completionCb(false, "Client already installed");
			return;
		}

		const std::string arch = GetHardwareArchitecture();

		if (arch == "aarch64" && clientName == "Delta") {
			if (progressCb) progressCb(0.0f, "Fetching Delta version...");

			std::string version;

			try {
				version = GetDeltaVersion();
			} catch (const std::exception& e) {
				LOG_ERROR("Failed to fetch Delta version: {}", e.what());
				if (completionCb) completionCb(false, "Failed to fetch Delta version");
				return;
			}

			if (version.empty()) {
				if (completionCb) completionCb(false, "Failed to fetch Delta version");
				return;
			}

			LOG_INFO("Installing Delta IPA version {}", version);

			bool success = IPAInstaller::InstallIPA(appDataDir, clientName, version, progressCb);

			if (success) {
				if (progressCb) progressCb(1.0f, "IPA installation complete!");
				LOG_INFO("Successfully installed {} IPA", clientName);
				if (completionCb) completionCb(true, "Installation successful");

				std::string clientVersion = ClientUpdateChecker::UpdateChecker::GetClientVersion(clientName);
				if (!clientVersion.empty()) {
					ClientUpdateChecker::UpdateChecker::MarkClientAsInstalled(clientName, clientVersion);
				}
			} else {
				LOG_ERROR("IPA installation failed");
				if (completionCb) completionCb(false, "IPA installation failed");
			}

			return;
		}

		// Original installation flow for non-IPA clients
		if (progressCb) progressCb(0.0f, "Fetching latest Roblox version...");

		std::string version;

		if (clientName == "Hydrogen") {
			try {
				HydrogenVersion ver = GetHydrogenVersion();
				version = ver.macos.roblox_version.value_or("");
			} catch (const std::exception& e) {
				LOG_WARN("Failed to fetch Hydrogen version: {}, falling back to latest", e.what());
			}
		} else if (clientName == "MacSploit") {
			try {
				MacsploitVersion ver = GetMacsploitVersion();
				version = ver.clientVersionUpload;
			} catch (const std::exception& e) {
				LOG_WARN("Failed to fetch MacSploit version: {}, falling back to latest", e.what());
			}
		}

		if (version.empty()) {
			version = GetLatestRobloxVersion();
		}

		if (version.empty()) {
			if (completionCb) completionCb(false, "Failed to fetch Roblox version");
			return;
		}

		LOG_INFO("Installing {} with Roblox version {}", clientName, version);

		const std::filesystem::path zipPath = clientsDir / std::format("{}-{}.zip", arch, version);

		std::error_code ec;
		std::filesystem::create_directories(clientsDir, ec);

		if (!std::filesystem::exists(zipPath)) {
			if (progressCb) progressCb(0.1f, "Downloading Roblox...");

			auto downloadProgress = [progressCb](float p, const std::string& msg) {
				if (progressCb) progressCb(0.1f + p * 0.4f, msg);
			};

			if (!DownloadRoblox(version, zipPath.string(), downloadProgress)) {
				if (completionCb) completionCb(false, "Failed to download Roblox");
				return;
			}
		}

		if (progressCb) progressCb(0.5f, "Extracting Roblox...");

		auto extractProgress = [progressCb](float p, const std::string& msg) {
			if (progressCb) progressCb(0.5f + p * 0.1f, msg);
		};

		if (!ExtractRoblox(zipPath.string(), clientsDir.string(), extractProgress)) {
			if (completionCb) completionCb(false, "Failed to extract Roblox");
			return;
		}

		if (progressCb) progressCb(0.6f, "Cleaning up...");
		if (!CleanupRobloxApp(clientsDir.string(), nullptr)) {
			LOG_WARN("Cleanup failed, continuing anyway");
		}

		const std::filesystem::path robloxPlayerPath = clientsDir / "RobloxPlayer.app";
		const std::filesystem::path executableDir = robloxPlayerPath / "Contents" / "MacOS";
		const std::filesystem::path binaryPath = executableDir / "RobloxPlayer";

		if (clientName != "Vanilla") {
			const std::filesystem::path insertDylibPath = std::filesystem::path(appDataDir) / "insert_dylib";

			if (!std::filesystem::exists(insertDylibPath)) {
				if (progressCb) progressCb(0.65f, "Downloading insert_dylib...");

				auto insertDylibProgress = [progressCb](float p, const std::string& msg) {
					if (progressCb) progressCb(0.65f + p * 0.05f, msg);
				};

				if (!DownloadInsertDylib(insertDylibPath.string(), insertDylibProgress)) {
					if (completionCb) completionCb(false, "Failed to download insert_dylib");
					return;
				}
			}

			if (progressCb) progressCb(0.7f, std::format("Downloading {} dylib...", clientName));

			std::string dylibName = clientName;
			std::transform(dylibName.begin(), dylibName.end(), dylibName.begin(),
						  [](unsigned char c) { return std::tolower(c); });
			dylibName += ".dylib";

			const std::filesystem::path dylibPath = executableDir / dylibName;

			auto dylibProgress = [progressCb](float p, const std::string& msg) {
				if (progressCb) progressCb(0.7f + p * 0.1f, msg);
			};

			if (!DownloadDylib(clientName, dylibPath.string(), dylibProgress)) {
				if (completionCb) completionCb(false, std::format("Failed to download {} dylib", clientName));
				return;
			}

			if (arch == "aarch64") {
				if (progressCb) progressCb(0.8f, "Removing signature...");
				if (!CodeSign(robloxPlayerPath.string(), true, nullptr)) {
					if (completionCb) completionCb(false, "Failed to remove signature");
					return;
				}
			}

			if (progressCb) progressCb(0.85f, "Injecting dylib...");

			auto insertProgress = [progressCb](float p, const std::string& msg) {
				if (progressCb) progressCb(0.85f + p * 0.1f, msg);
			};

			if (!InsertDylib(insertDylibPath.string(), dylibPath.string(),
						   binaryPath.string(), insertProgress)) {
				if (completionCb) completionCb(false, "Failed to inject dylib");
				return;
			}
		}

		if (progressCb) progressCb(0.95f, "Signing app...");
		if (!CodeSign(robloxPlayerPath.string(), false, nullptr)) {
			if (completionCb) completionCb(false, "Failed to sign app");
			return;
			}
				std::filesystem::rename(robloxPlayerPath, finalAppPath, ec);
				if (ec) {
					if (completionCb) completionCb(false, std::format("Failed to rename app: {}", ec.message()));
					return;
				}

				std::filesystem::remove(zipPath, ec);
				if (ec) {
					LOG_WARN("Failed to cleanup zip file: {}", ec.message());
				}

				if (progressCb) progressCb(1.0f, "Installation complete!");
				LOG_INFO("Successfully installed {}", clientName);

				if (completionCb) completionCb(true, "Installation successful");

				std::string clientVersion = ClientUpdateChecker::UpdateChecker::GetClientVersion(clientName);
				if (!clientVersion.empty()) {
					ClientUpdateChecker::UpdateChecker::MarkClientAsInstalled(clientName, clientVersion);
				}
			}).detach();
}
void RemoveClientAsync(const std::string& clientName, CompletionCallback completionCb) {
	std::thread([clientName, completionCb] {
	const std::string clientPath = MultiInstance::getBaseClientPath(clientName);
	if (clientPath.empty() || !std::filesystem::exists(clientPath)) {
	if (completionCb) completionCb(false, "Client not found");
	return;
	}
		std::error_code ec;
		std::filesystem::remove_all(clientPath, ec);

		if (ec) {
			LOG_ERROR("Failed to remove client: {}", ec.message());
			if (completionCb) completionCb(false, std::format("Failed to remove: {}", ec.message()));
			return;
		}

		LOG_INFO("Removed client: {}", clientName);
		if (completionCb) completionCb(true, "Client removed successfully");
	}).detach();
}
}