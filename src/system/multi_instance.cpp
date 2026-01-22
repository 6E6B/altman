#include "multi_instance.h"

#ifdef _WIN32
#include <filesystem>
#include <windows.h>
#include <shlobj.h>
#include "console/console.h"

namespace MultiInstance {
    namespace {
        HANDLE g_mutex = nullptr;
    }

    void Enable() {
        if (!g_mutex)
            g_mutex = CreateMutexW(nullptr, FALSE, L"ROBLOX_singletonEvent");
    }

    void Disable() {
        if (g_mutex) {
            CloseHandle(g_mutex);
            g_mutex = nullptr;
        }
    }
}

#elif __APPLE__
#include <semaphore.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <spawn.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <array>
#include <map>
#include <mutex>
#include <vector>
#include <algorithm>
#include <format>

#include "console/console.h"
#include "utils/paths.h"
#include "system/system_info.h"

namespace MultiInstance {
    namespace {
        sem_t* g_semaphore = SEM_FAILED;
        const char* SEMAPHORE_NAME = "/RobloxPlayerUniq";
        std::map<pid_t, RobloxInstance> g_activeInstances;
        std::mutex g_instanceMutex;
    }

    void Enable() {
        /*if (g_semaphore == SEM_FAILED) {
            g_semaphore = sem_open(SEMAPHORE_NAME, O_CREAT, 0644, 1);
            if (g_semaphore == SEM_FAILED) {
                return;
            }
        }*/
    }

    void Disable() {
        /*if (g_semaphore != SEM_FAILED) {
            sem_close(g_semaphore);
            sem_unlink(SEMAPHORE_NAME);
            g_semaphore = SEM_FAILED;
        }*/
    }

    std::string getUserClientPath(const std::string& username, const std::string& clientName) {
        auto appDataDir = AltMan::Paths::AppData();
        if (appDataDir.empty()) return "";

        return std::format("{}/environments/{}/Applications/{}.app",
            appDataDir.string(), username, clientName);
    }

    namespace {
        std::optional<std::string> getBundleIdentifier(const std::filesystem::path& appBundle) {
            std::filesystem::path plistPath = appBundle / "Contents" / "Info.plist";

            if (!std::filesystem::exists(plistPath)) {
                return std::nullopt;
            }

            try {
                std::ifstream file(plistPath);
                std::string contents((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());

                std::regex bundleIdRegex("<key>CFBundleIdentifier</key>\\s*<string>([^<]+)</string>");
                std::smatch match;

                if (std::regex_search(contents, match, bundleIdRegex) && match.size() > 1) {
                    return match[1].str();
                }
            } catch (const std::exception& e) {
                LOG_ERROR("Error reading bundle identifier: {}", e.what());
            }

            return std::nullopt;
        }

        std::optional<std::filesystem::path> findMainExecutable(const std::filesystem::path& appBundle) {
            std::filesystem::path macosDir = appBundle / "Contents" / "MacOS";

            if (!std::filesystem::exists(macosDir)) {
                return std::nullopt;
            }

            const std::vector<std::string> commonNames = {"Roblox", "RobloxPlayer"};
            for (const auto& name : commonNames) {
                std::filesystem::path candidate = macosDir / name;
                if (std::filesystem::exists(candidate)) {
                    return candidate;
                }
            }

            try {
                for (const auto& entry : std::filesystem::directory_iterator(macosDir)) {
                    if (!entry.is_regular_file()) {
                        continue;
                    }

                    auto perms = entry.status().permissions();
                    if ((perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none) {
                        return entry.path();
                    }
                }
            } catch (const std::exception& e) {
                LOG_ERROR("Error finding executable: {}", e.what());
            }

            return std::nullopt;
        }

        struct mach_header_64 {
            uint32_t magic;
            uint32_t cputype;
            uint32_t cpusubtype;
            uint32_t filetype;
            uint32_t ncmds;
            uint32_t sizeofcmds;
            uint32_t flags;
            uint32_t reserved;
        };

        struct load_command {
            uint32_t cmd;
            uint32_t cmdsize;
        };

        struct segment_command_64 {
            uint32_t cmd;
            uint32_t cmdsize;
            char segname[16];
            uint64_t vmaddr;
            uint64_t vmsize;
            uint64_t fileoff;
            uint64_t filesize;
            uint32_t maxprot;
            uint32_t initprot;
            uint32_t nsects;
            uint32_t flags;
        };

        constexpr uint32_t MH_MAGIC_64 = 0xfeedfacf;
        constexpr uint32_t LC_SEGMENT_64 = 0x19;

        uint64_t computeCodeHash(const std::filesystem::path& filePath) {
            std::ifstream file(filePath, std::ios::binary);
            if (!file) {
                throw std::runtime_error("Cannot open file for hashing");
            }

            mach_header_64 header;
            file.read(reinterpret_cast<char*>(&header), sizeof(header));

            if (header.magic != MH_MAGIC_64) {
                throw std::runtime_error("Not a 64-bit Mach-O file");
            }

            uint64_t hash = 14695981039346656037ULL;
            const uint64_t prime = 1099511628211ULL;

            for (uint32_t i = 0; i < header.ncmds; ++i) {
                load_command cmd;
                auto cmdPos = file.tellg();
                file.read(reinterpret_cast<char*>(&cmd), sizeof(cmd));

                if (cmd.cmd == LC_SEGMENT_64) {
                    file.seekg(cmdPos);
                    segment_command_64 segment;
                    file.read(reinterpret_cast<char*>(&segment), sizeof(segment));

                    if (std::string(segment.segname, 16).find("__TEXT") == 0) {
                        auto currentPos = file.tellg();
                        file.seekg(segment.fileoff);

                        constexpr size_t BUFFER_SIZE = 64 * 1024;
                        std::vector<char> buffer(BUFFER_SIZE);
                        uint64_t remaining = segment.filesize;

                        while (remaining > 0) {
                            size_t toRead = std::min(remaining, static_cast<uint64_t>(BUFFER_SIZE));
                            file.read(buffer.data(), toRead);
                            size_t bytesRead = file.gcount();

                            for (size_t j = 0; j < bytesRead; ++j) {
                                hash ^= static_cast<uint8_t>(buffer[j]);
                                hash *= prime;
                            }

                            remaining -= bytesRead;
                        }

                        file.seekg(currentPos);
                        break;
                    }
                }

                file.seekg(static_cast<std::streamoff>(cmdPos) + cmd.cmdsize);
            }

            return hash;
        }

        bool isMobileClient2(std::string_view clientPath) {
            std::filesystem::path libgloopMarker =
                std::filesystem::path(clientPath) / "Contents" / "Frameworks" / "libgloop.dylib";
            return std::filesystem::exists(libgloopMarker);
        }
    }

    void saveSourceHash(const std::string& destPath) {
        try {
            auto destExecOpt = findMainExecutable(destPath);
            if (!destExecOpt) {
                LOG_ERROR("Cannot save hash - executable not found");
                return;
            }

            uint64_t hash = computeCodeHash(*destExecOpt);

            std::filesystem::path hashFile = std::filesystem::path(destPath).string() + ".hash";
            std::ofstream hashOut(hashFile);
            hashOut << std::hex << hash;

        } catch (const std::exception& e) {
            LOG_ERROR("Error saving source hash: {}", e.what());
        }
    }

    bool needsClientUpdate(const std::string& sourcePath, const std::string& destPath) {
        if (!std::filesystem::exists(destPath)) {
            return true;
        }

        try {
            auto sourceExecOpt = findMainExecutable(sourcePath);
            if (!sourceExecOpt) {
                LOG_ERROR("Source executable not found in {}", sourcePath);
                return false;
            }
            std::filesystem::path sourceExec = *sourceExecOpt;

            auto destExecOpt = findMainExecutable(destPath);
            if (!destExecOpt) {
                return true;
            }

            std::filesystem::path hashFile = std::filesystem::path(destPath).string() + ".hash";

            uint64_t sourceHash = computeCodeHash(sourceExec);

            if (std::filesystem::exists(hashFile)) {
                std::ifstream hashIn(hashFile);
                uint64_t storedHash;
                hashIn >> std::hex >> storedHash;

                if (sourceHash == storedHash) {
                    return false;
                } else {
                    return true;
                }
            }
            return true;
        } catch (const std::exception& e) {
            LOG_ERROR("Error checking client update: {}", e.what());
            return false;
        }
    }

    bool cleanupUserEnvironment(const std::string& username) {
    	auto appDataDir = AltMan::Paths::AppData();
        if (appDataDir.empty()) return false;

        std::filesystem::path userEnvPath = appDataDir / "environments" / username;

        if (!std::filesystem::exists(userEnvPath)) {
            return true;
        }

        std::error_code ec;
        std::filesystem::remove_all(userEnvPath, ec);

        if (ec) {
            LOG_ERROR("Failed to cleanup user environment: {}", ec.message());
            return false;
        }

        return true;
    }

    bool isClientInstalled(const std::string& username, const std::string& clientName) {
        std::string clientPath = getUserClientPath(username, clientName);
        return !clientPath.empty() && std::filesystem::exists(clientPath);
    }

    bool isBaseClientInstalled(const std::string& clientName) {
    	auto appDataDir = AltMan::Paths::AppData();
        if (appDataDir.empty()) return false;

        std::filesystem::path clientPath = appDataDir / "clients" / (clientName + ".app");
        return std::filesystem::exists(clientPath);
    }

	const std::vector<std::string>& getAvailableClients(bool forceRefresh) {
	    static std::vector<std::string> cached;
    	static std::filesystem::file_time_type lastScan{};
    	static bool initialized = false;
    	static std::mutex m;

    	std::lock_guard<std::mutex> lock(m);

    	auto appDataDir = AltMan::Paths::AppData();
    	if (appDataDir.empty()) return cached;

    	std::filesystem::path clientsDir = appDataDir / "clients";

    	std::error_code ec;
    	bool exists = std::filesystem::exists(clientsDir, ec);
    	auto currentWriteTime = exists
			? std::filesystem::last_write_time(clientsDir, ec)
			: std::filesystem::file_time_type{};

    	if (!initialized || forceRefresh || currentWriteTime != lastScan) {
    		cached.clear();

    		if (exists) {
    			for (const auto& entry : std::filesystem::directory_iterator(clientsDir, ec)) {
    				const auto& path = entry.path();
    				if (entry.is_directory() && path.extension() == ".app") {
    					cached.push_back(path.stem().string());
    				}
    			}
    		}

    		std::sort(cached.begin(), cached.end());
    		lastScan = currentWriteTime;
    		initialized = true;
    	}

    	return cached;
    }

	const std::vector<std::string>& getAvailableClientsForUI(bool refresh) {
    	static std::vector<std::string> cached;

    	if (refresh || cached.empty()) {
    		cached.clear();
    		cached.push_back("Default");

    		for (const auto& c : getAvailableClients(refresh)) {
    			if (c != "Default") {
    				cached.push_back(c);
    			}
    		}
    	}

    	return cached;
    }

    std::string getClientPath(const std::string& username, const std::string& clientName) {
        return getUserClientPath(username, clientName);
    }

    std::string getBaseClientPath(const std::string& clientName) {
    	auto appDataDir = AltMan::Paths::AppData();
        if (appDataDir.empty()) return "";

        std::string baseClientName = clientName;
        if (clientName.find("Roblox_") == 0) {
            baseClientName = "Default";
        }

        return std::format("{}/clients/{}.app", appDataDir.string(), baseClientName);
    }

	bool createKeychain(const std::string& profileId) {
    	auto appDataDir = AltMan::Paths::AppData();
    	if (appDataDir.empty())
    		return false;

    	auto profileDir = appDataDir / "environments" / profileId;

    	auto keychainDir = profileDir / "Library" / "Keychains";
    	std::error_code ec;
    	std::filesystem::create_directories(keychainDir, ec);

    	auto keychainPath = keychainDir / "login.keychain-db";

    	if (std::filesystem::exists(keychainPath)) {
    		return true;
    	}

    	return SystemInfo::SpawnWithCustomHome("/usr/bin/security",
			{"security", "create-keychain", "-p", "", keychainPath.string().c_str()},
			profileDir.string()
		);
    }

	bool unlockKeychain(const std::string& profileId) {
    	auto appDataDir = AltMan::Paths::AppData();
    	if (appDataDir.empty())
    		return false;

    	auto profileDir = appDataDir / "environments" / profileId;

    	auto keychainPath = profileDir / "Library" / "Keychains" / "login.keychain-db";

    	if (!std::filesystem::exists(keychainPath)) {
    		LOG_INFO("Keychain does not exist: {}", keychainPath.string());
    		return false;
    	}

    	return SystemInfo::SpawnWithCustomHome(
			"/usr/bin/security",
			{"security", "unlock-keychain", "-p", "", keychainPath.string().c_str()},
			profileDir.string()
		);
    }

    bool createProfileEnvironment(const std::string& profileId, std::string& profilePath) {
    	auto appDataDir = AltMan::Paths::AppData();
        if (appDataDir.empty()) return false;

        std::filesystem::path environmentsDir = appDataDir / "environments" / profileId;

        std::error_code ec;
        std::filesystem::create_directories(environmentsDir, ec);
        if (ec) {
            LOG_ERROR("Failed to create profile environment: {}", ec.message());
            return false;
        }

        std::filesystem::create_directories(environmentsDir / "Documents", ec);
        std::filesystem::create_directories(environmentsDir / "Applications", ec);
        std::filesystem::create_directories(environmentsDir / "Library", ec);
        std::filesystem::create_directories(environmentsDir / "Library" / "Preferences", ec);
        std::filesystem::create_directories(environmentsDir / "Library" / "Keychains", ec);
        std::filesystem::create_directories(environmentsDir / "Library" / "Application Support", ec);
        std::filesystem::create_directories(environmentsDir / "Library" / "Caches", ec);
        std::filesystem::create_directories(environmentsDir / "Library" / "Delta" / "Cache", ec);

        std::filesystem::create_directories(environmentsDir / "Documents" / "Delta" / "Autoexecute", ec);
        std::filesystem::create_directories(environmentsDir / "Documents" / "Delta" / "Scripts", ec);
        std::filesystem::create_directories(environmentsDir / "Documents" / "Delta" / "Workspace", ec);

        std::filesystem::create_directories(environmentsDir / "Hydrogen", ec);
        std::filesystem::create_directories(environmentsDir / "Hydrogen" / "autoexecute", ec);
        std::filesystem::create_directories(environmentsDir / "Hydrogen" / "workspace", ec);

        std::filesystem::create_directories(environmentsDir / "Documents" / "Macsploit Automatic Execution", ec);
        std::filesystem::create_directories(environmentsDir / "Documents" / "Macsploit Workspace", ec);

        if (ec) {
            LOG_ERROR("Failed to create profile subdirectories: {}", ec.message());
            return false;
        }

        profilePath = environmentsDir.string();
        return true;
    }

    bool modifyBundleIdentifier(const std::string& username,
                            const std::string& clientName,
                            const std::string& profileId,
                            bool isInitialSetup) {
        const std::string clientPath = getUserClientPath(username, clientName);
        if (clientPath.empty() || !std::filesystem::exists(clientPath)) {
            LOG_ERROR("Client not found: {}", clientName);
            return false;
        }

        std::string plistPath = std::format("{}/Contents/Info.plist", clientPath);
        if (!std::filesystem::exists(plistPath)) {
            LOG_ERROR("Info.plist not found");
            return false;
        }

        std::ifstream plistIn(plistPath);
        if (!plistIn) {
            LOG_ERROR("Failed to open Info.plist for reading");
            return false;
        }

        std::string plistContent((std::istreambuf_iterator<char>(plistIn)),
                                 std::istreambuf_iterator<char>());
        plistIn.close();

        const std::regex bundleIdRegex(R"(<string>com\.roblox\.RobloxPlayer\.?\w*</string>)");
        const std::string newBundleId = profileId.empty()
            ? "<string>com.roblox.RobloxPlayer</string>"
            : std::format("<string>com.roblox.RobloxPlayer.{}</string>", profileId);

        plistContent = std::regex_replace(plistContent, bundleIdRegex, newBundleId);

        {
            std::ofstream plistOut(plistPath, std::ios::out | std::ios::trunc);
            if (!plistOut) {
                LOG_ERROR("Failed to open Info.plist for writing");
                return false;
            }
            plistOut << plistContent;
            plistOut.flush();
            plistOut.close();
        }

        std::string codesignCmd = std::format(
            "codesign --force --deep -s - \"{}\" 2>&1",
            clientPath
        );

    	std::string output;
        if (!SystemInfo::ExecuteCommand(codesignCmd, output)) {
            LOG_ERROR("Codesign failed {}", output);
            return false;
        }

        return true;
    }

    bool needsBundleIdModification(const std::string& username,
                                  const std::string& clientName,
                                  const std::string& expectedProfileId) {
        const std::string clientPath = getUserClientPath(username, clientName);
        if (clientPath.empty() || !std::filesystem::exists(clientPath)) {
            return false;
        }

        std::string plistPath = std::format("{}/Contents/Info.plist", clientPath);
        if (!std::filesystem::exists(plistPath)) {
            return true;
        }

        std::ifstream plistIn(plistPath);
        if (!plistIn) {
            return true;
        }

        std::string plistContent((std::istreambuf_iterator<char>(plistIn)),
                                 std::istreambuf_iterator<char>());
        plistIn.close();

        const std::string expectedBundleId = std::format(
            "<string>com.roblox.RobloxPlayer.{}</string>", expectedProfileId);

        return plistContent.find(expectedBundleId) == std::string::npos;
    }

    bool ensureClientKey(const std::string& username, const std::string& clientName, const std::string& key) {
        if (key.empty()) {
            LOG_ERROR("Key required for {} but not provided", clientName);
            return false;
        }

    	auto appDataDir = AltMan::Paths::AppData();
        if (appDataDir.empty()) return false;

        std::string keyPath;
        if (clientName == "Hydrogen") {
            keyPath = std::format("{}/environments/{}/Library/Application Support/Hydrogen/Key.txt",
                appDataDir.string(), username);
        } else if (clientName == "Delta") {
            keyPath = std::format("{}/environments/{}/Library/Delta/Cache/license",
                appDataDir.string(), username);
        } else {
            return true;
        }

        std::filesystem::path keyDir = std::filesystem::path(keyPath).parent_path();
        std::error_code ec;
        std::filesystem::create_directories(keyDir, ec);
        if (ec) {
            LOG_ERROR("Failed to create key directory for {}: {}", clientName, ec.message());
            return false;
        }

        if (std::filesystem::exists(keyPath)) {
	        std::ifstream existingKeyFile(keyPath);
        	if (existingKeyFile) {
        		std::string existingKey((std::istreambuf_iterator<char>(existingKeyFile)),
				std::istreambuf_iterator<char>());
        		existingKeyFile.close();
        		if (existingKey == key) {
        			return true;
        		}
        	}
        }

    std::ofstream keyFile(keyPath);
    if (!keyFile) {
        LOG_ERROR("Failed to write key file for {}", clientName);
        return false;
    }

    keyFile << key;
    keyFile.close();
    return true;
}

bool launchSandboxedClient(const std::string& username,
						   const std::string& clientName,
						   const std::string& profileId,
						   const std::string& profilePath,
						   const std::string& protocolURL) {
    std::string clientPath = getUserClientPath(username, clientName);
    if (clientPath.empty() || !std::filesystem::exists(clientPath)) {
    	LOG_ERROR("Client not installed: {}", clientName);
    	return false;
    }

    bool mobileClient = isMobileClient2(clientPath);

    std::string logDir = std::format("{}/Logs", profilePath);
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);

    std::vector<const char*> argv = {
    	"open",
		"-a",
		clientPath.c_str()
	};

    if (!protocolURL.empty()) {
    	argv.push_back(protocolURL.c_str());
    }

    SystemInfo::SpawnOptions opts;
    opts.env = {
    	{"HOME", profilePath},
    	// uses up too much space for desktop but fixes keychain thing
    	{"CFFIXED_USER_HOME", profilePath}
    };

    if (mobileClient) {
    	//opts.env["CFFIXED_USER_HOME"] = profilePath;
    	opts.env["XDG_DATA_HOME"] = profilePath + "/Documents";
    }

    opts.stdoutPath = logDir + "/roblox_stdout.log";
    opts.stderrPath = logDir + "/roblox_stderr.log";
    opts.waitForCompletion = false;

    if (!SystemInfo::SpawnProcessWithEnv("/usr/bin/open", argv, opts)) {
    	LOG_ERROR("Failed to launch client");
    	return false;
    }

    return true;
}

bool stopSandboxedRoblox(pid_t pid) {
    if (pid <= 0) {
        LOG_ERROR("Invalid PID");
        return false;
    }

    LOG_INFO("Stopping client instance with PID: {}", pid);

    if (kill(pid, SIGTERM) == 0) {
        LOG_INFO("Sent SIGTERM to process {}", pid);

        for (int i = 0; i < 50; i++) {
            if (kill(pid, 0) != 0) {
                LOG_INFO("Process terminated gracefully");
                return true;
            }
            usleep(100000);
        }

        LOG_INFO("Process didn't terminate gracefully, sending SIGKILL");
        if (kill(pid, SIGKILL) == 0) {
            LOG_INFO("Sent SIGKILL to process {}", pid);
            return true;
        }
    }

    LOG_ERROR("Failed to stop process: {}", strerror(errno));
    return false;
}

void parsePattern(const std::string& patternStr, std::vector<uint8_t>& patternBytes, std::vector<uint8_t>& mask) {
    std::stringstream ss(patternStr);
    std::string byteStr;

    while (ss >> byteStr) {
    	if (byteStr == "?") {
    		patternBytes.push_back(0x00);
    		mask.push_back(0x00);
    	} else {
    		patternBytes.push_back(static_cast<uint8_t>(std::stoul(byteStr, nullptr, 16)));
    		mask.push_back(0xFF);
    	}
    }
}

bool comparePattern(const uint8_t* data, const std::vector<uint8_t>& pattern, const std::vector<uint8_t>& mask) {
    for (size_t i = 0; i < pattern.size(); ++i) {
    	if ((data[i] & mask[i]) != pattern[i]) {
    		return false;
    	}
    }
    return true;
}

bool patchRobloxBinary(const std::string& appPath) {
    std::string binaryPath = appPath + "/Contents/MacOS/RobloxPlayer";

    std::vector<PatchTarget> targets = {
        {
        	"RobloxTerminationRoutine -> CBZ check",
            "F3 03 00 AA F3 03 00 F9 ? ? 02 ? ? ? ? 91 ? ? ? ?",
            {0x07, 0x00, 0x00, 0x14},
            0x18
        },
        {
        	"RobloxTerminationRoutine -> _objc_msgSend$terminate_",
            "00 01 40 F9 02 00 80 D2 FD 7B 42 A9 F4 4F 41 A9 FF C3 00 91",
            {0x1F, 0x20, 0x03, 0xD5},
            0x14
        },
        {
        	"signalShutdownSemaphore -> sem_post",
            "? ? ? 91 ? ? ? ? ? ? 02 ? 1F 61 30 39 ? ? ? F9 FD 7B 42 A9 F4 4F 41 A9",
            {0x1F, 0x20, 0x03, 0xD5},
            0x24
        }
    };

    std::ifstream binaryFile(binaryPath, std::ios::binary | std::ios::ate);
    if (!binaryFile.is_open()) {
        LOG_ERROR("Failed to open RobloxPlayer binary");
        return false;
    }

    std::streamsize size = binaryFile.tellg();
    std::vector<uint8_t> data(size);
    binaryFile.seekg(0, std::ios::beg);
    binaryFile.read(reinterpret_cast<char*>(data.data()), size);
    binaryFile.close();

    int totalPatchesApplied = 0;

    for (size_t targetIndex = 0; targetIndex < targets.size(); targetIndex++) {
        PatchTarget& target = targets[targetIndex];

        std::vector<uint8_t> patternBytes;
        std::vector<uint8_t> mask;
        parsePattern(target.patternStr, patternBytes, mask);

        size_t patchAddress = std::string::npos;

        if (data.size() >= patternBytes.size()) {
            for (size_t i = 0; i <= data.size() - patternBytes.size(); ++i) {
                if (comparePattern(&data[i], patternBytes, mask)) {
                    patchAddress = i;
                    break;
                }
            }
        }

        patchAddress += target.offset;

        if (patchAddress == std::string::npos) {
            LOG_INFO("Target {}: Pattern not found. Skipping.", targetIndex + 1);
            continue;
        }

        if (std::memcmp(&data[patchAddress], target.patchBytes.data(), target.patchBytes.size()) == 0) {
            LOG_INFO("Target {}: Already patched. Skipping.", targetIndex + 1);
            continue;
        }

        std::memcpy(&data[patchAddress], target.patchBytes.data(), target.patchBytes.size());
        totalPatchesApplied++;
        LOG_INFO("Target {}: Successfully patched instruction at 0x{:x}",
                  targetIndex + 1, patchAddress);
    }

    if (totalPatchesApplied == 0) {
        LOG_INFO("Patches already applied or no patterns found.");
        return true;
    }

    std::ofstream output(binaryPath, std::ios::binary);
    if (!output.is_open()) {
        LOG_ERROR("Failed to open binary for writing.");
        return false;
    }

    output.write(reinterpret_cast<const char*>(data.data()), data.size());
    output.close();

    LOG_INFO("Binary successfully processed. Total patches applied: {}", totalPatchesApplied);
    return true;
}

}
#endif