#include "multi_instance.h"

#ifdef _WIN32
#include <windows.h>

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
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
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

extern char** environ;

namespace MultiInstance {
    namespace {
        sem_t* g_semaphore = SEM_FAILED;
        const char* SEMAPHORE_NAME = "/RobloxPlayerUniq";
        std::map<pid_t, RobloxInstance> g_activeInstances;
        std::mutex g_instanceMutex;
    }

    void Enable() {
        if (g_semaphore == SEM_FAILED) {
            g_semaphore = sem_open(SEMAPHORE_NAME, O_CREAT, 0644, 1);
            if (g_semaphore == SEM_FAILED) {
                return;
            }
        }
    }

    void Disable() {
        if (g_semaphore != SEM_FAILED) {
            sem_close(g_semaphore);
            sem_unlink(SEMAPHORE_NAME);
            g_semaphore = SEM_FAILED;
        }
    }

    std::pair<int, std::string> executeCommand(const std::string& cmd) {
        std::array<char, 128> buffer;
        std::string result;

        FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
        if (!pipe) return {-1, "popen failed"};

        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            result += buffer.data();
        }

        int exitCode = pclose(pipe);
        return {WEXITSTATUS(exitCode), result};
    }

    std::string getAppDataDirectory() {
        const char* home = getenv("HOME");
        if (!home) {
            LOG_ERROR("Failed to get HOME directory");
            return "";
        }

        std::filesystem::path appDataDir = std::filesystem::path(home) / "Library" / "Application Support" / "Altman";

        std::error_code ec;
        std::filesystem::create_directories(appDataDir, ec);
        if (ec) {
            LOG_ERROR("Failed to create app data directory: {}", ec.message());
            return "";
        }

        return appDataDir.string();
    }

    std::string getUserClientPath(const std::string& username, const std::string& clientName) {
        std::string appDataDir = getAppDataDirectory();
        if (appDataDir.empty()) return "";

        return std::format("{}/environments/{}/Applications/{}.app",
            appDataDir, username, clientName);
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

        struct SpawnOptions {
            std::map<std::string, std::string> env;
            std::optional<std::string> stdoutPath;
            std::optional<std::string> stderrPath;
            bool waitForCompletion = true;
        };

        bool spawnProcessWithEnv(
            const char* program,
            const std::vector<const char*>& args,
            const SpawnOptions& opts) {
            std::vector<std::string> envStorage;
            std::vector<char*> envp;

            for (char** env = environ; *env; ++env) {
                std::string envStr(*env);
                size_t eqPos = envStr.find('=');
                if (eqPos != std::string::npos) {
                    std::string key = envStr.substr(0, eqPos);
                    if (opts.env.find(key) == opts.env.end()) {
                        envStorage.emplace_back(envStr);
                    }
                }
            }

            for (const auto& [key, value] : opts.env) {
                envStorage.emplace_back(key + "=" + value);
            }

            for (auto& s : envStorage) {
                envp.push_back(s.data());
            }
            envp.push_back(nullptr);

            std::vector<char*> argv;
            for (auto* arg : args) {
                argv.push_back(const_cast<char*>(arg));
            }
            argv.push_back(nullptr);

            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_t* actionsPtr = nullptr;

            if (opts.stdoutPath || opts.stderrPath) {
                posix_spawn_file_actions_init(&actions);
                actionsPtr = &actions;

                if (opts.stdoutPath) {
                    int outFd = open(opts.stdoutPath->c_str(),
                                O_CREAT | O_WRONLY | O_APPEND, 0644);
                    if (outFd != -1) {
                        posix_spawn_file_actions_adddup2(&actions, outFd, STDOUT_FILENO);
                        posix_spawn_file_actions_addclose(&actions, outFd);
                    }
                }

                if (opts.stderrPath) {
                    int errFd = open(opts.stderrPath->c_str(),
                                O_CREAT | O_WRONLY | O_APPEND, 0644);
                    if (errFd != -1) {
                        posix_spawn_file_actions_adddup2(&actions, errFd, STDERR_FILENO);
                        posix_spawn_file_actions_addclose(&actions, errFd);
                    }
                }
            }

            pid_t pid;
            int status = posix_spawn(&pid, program, actionsPtr, nullptr,
                                argv.data(), envp.data());

            if (actionsPtr) {
                posix_spawn_file_actions_destroy(&actions);
            }

            if (status != 0) {
                LOG_ERROR("posix_spawn failed: {}", strerror(status));
                return false;
            }

            if (opts.waitForCompletion) {
                int wstatus;
                waitpid(pid, &wstatus, 0);
                if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
                    return false;
                }
            }

            return true;
        }

        bool spawnWithEnv(const char* program,
                    const std::vector<const char*>& args,
                    const std::string& customHome) {
            SpawnOptions opts;
            opts.env = {{"HOME", customHome}};
            opts.waitForCompletion = true;
            return spawnProcessWithEnv(program, args, opts);
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

            LOG_INFO("Saved source hash: {:016x}", hash);
        } catch (const std::exception& e) {
            LOG_ERROR("Error saving source hash: {}", e.what());
        }
    }

    bool needsClientUpdate(const std::string& sourcePath, const std::string& destPath) {
        if (!std::filesystem::exists(destPath)) {
            LOG_INFO("Destination does not exist, needs update");
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
                LOG_INFO("Destination executable does not exist, needs update");
                return true;
            }

            std::filesystem::path hashFile = std::filesystem::path(destPath).string() + ".hash";

            uint64_t sourceHash = computeCodeHash(sourceExec);
            LOG_INFO("Source hash: {:016x}", sourceHash);

            if (std::filesystem::exists(hashFile)) {
                std::ifstream hashIn(hashFile);
                uint64_t storedHash;
                hashIn >> std::hex >> storedHash;

                LOG_INFO("Stored hash: {:016x}", storedHash);

                if (sourceHash == storedHash) {
                    LOG_INFO("Source matches stored hash, destination is up to date");
                    return false;
                } else {
                    LOG_INFO("Source differs from stored hash, needs update");
                    return true;
                }
            }

            LOG_INFO("No stored hash found, needs update");
            return true;

        } catch (const std::exception& e) {
            LOG_ERROR("Error checking client update: {}", e.what());
            return false;
        }
    }

    bool cleanupUserEnvironment(const std::string& username) {
        std::string appDataDir = getAppDataDirectory();
        if (appDataDir.empty()) return false;

        std::filesystem::path userEnvPath = std::filesystem::path(appDataDir) / "environments" / username;

        if (!std::filesystem::exists(userEnvPath)) {
            LOG_INFO("User environment doesn't exist: {}", username);
            return true;
        }

        LOG_INFO("Cleaning up environment for user: {}", username);

        std::error_code ec;
        std::filesystem::remove_all(userEnvPath, ec);

        if (ec) {
            LOG_ERROR("Failed to cleanup user environment: {}", ec.message());
            return false;
        }

        LOG_INFO("User environment cleaned up successfully");
        return true;
    }

    bool isClientInstalled(const std::string& username, const std::string& clientName) {
        std::string clientPath = getUserClientPath(username, clientName);
        return !clientPath.empty() && std::filesystem::exists(clientPath);
    }

    bool isBaseClientInstalled(const std::string& clientName) {
        std::string appDataDir = getAppDataDirectory();
        if (appDataDir.empty()) return false;

        std::filesystem::path clientPath = std::filesystem::path(appDataDir) / "clients" / (clientName + ".app");
        return std::filesystem::exists(clientPath);
    }

    const std::vector<std::string>& getAvailableClients(bool forceRefresh) {
        static std::vector<std::string> cached;
        static std::filesystem::file_time_type lastScan{};
        static bool initialized = false;

        std::string appDataDir = getAppDataDirectory();
        if (appDataDir.empty()) return cached;

        std::filesystem::path clientsDir = std::filesystem::path(appDataDir) / "clients";

        std::error_code ec;
        auto currentWriteTime =
            std::filesystem::exists(clientsDir, ec)
                ? std::filesystem::last_write_time(clientsDir, ec)
                : std::filesystem::file_time_type{};

        if (!initialized || forceRefresh || currentWriteTime != lastScan) {
            cached.clear();

            if (std::filesystem::exists(clientsDir)) {
                for (const auto& entry : std::filesystem::directory_iterator(clientsDir, ec)) {
                    if (entry.is_directory() && entry.path().extension() == ".app") {
                        cached.push_back(entry.path().stem().string());
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
            cached.push_back("Vanilla");

            auto diskClients = getAvailableClients(false);
            for (const auto& c : diskClients) {
                if (c != "Vanilla") {
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
        std::string appDataDir = getAppDataDirectory();
        if (appDataDir.empty()) return "";

        std::string baseClientName = clientName;
        if (clientName.find("Roblox_") == 0) {
            baseClientName = "Vanilla";
        }

        return std::format("{}/clients/{}.app", appDataDir, baseClientName);
    }

    bool createKeychain(const std::string& profileId) {
        std::string appDataDir = getAppDataDirectory();
        if (appDataDir.empty()) return false;

        auto profileDir = std::filesystem::path(appDataDir) / "environments" / profileId;

        std::error_code ec;
        std::filesystem::create_directories(profileDir, ec);
        if (ec) {
            LOG_ERROR("Failed to create profile directory: {}", ec.message());
            return false;
        }

        LOG_INFO("Creating keychain for profile: {}", profileId);

        return spawnWithEnv("/usr/bin/security",
            {"security", "create-keychain", "-p", "", "login.keychain", nullptr},
            profileDir.string());
    }

    bool unlockKeychain(const std::string& profileId) {
        std::string appDataDir = getAppDataDirectory();
        if (appDataDir.empty()) return false;

        auto profileDir = std::filesystem::path(appDataDir) / "environments" / profileId;

        if (!std::filesystem::exists(profileDir)) {
            LOG_ERROR("Profile directory does not exist: {}", profileDir.string());
            return false;
        }

        LOG_INFO("Unlocking keychain for profile: {}", profileId);

        return spawnWithEnv("/usr/bin/security",
            {"security", "unlock-keychain", "-p", "", "login.keychain", nullptr},
            profileDir.string());
    }

    bool createProfileEnvironment(const std::string& profileId, std::string& profilePath) {
        std::string appDataDir = getAppDataDirectory();
        if (appDataDir.empty()) return false;

        std::filesystem::path environmentsDir = std::filesystem::path(appDataDir) / "environments" / profileId;

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
        LOG_INFO("Profile environment created: {}", profilePath);
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

        LOG_INFO("Modifying bundle identifier for client: {}", clientName);
        LOG_INFO("Profile ID: {}", profileId);

        std::string plistPath = std::format("{}/Contents/Info.plist", clientPath);
        if (!std::filesystem::exists(plistPath)) {
            LOG_ERROR("Info.plist not found");
            return false;
        }

        LOG_INFO("Using plist at: {}", plistPath);

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

        auto [result, output] = executeCommand(codesignCmd);

        if (result != 0) {
            LOG_ERROR("Codesign failed with code {}: {}", result, output);
            return false;
        }

        LOG_INFO("Codesigning completed successfully");
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

        std::string appDataDir = getAppDataDirectory();
        if (appDataDir.empty()) return false;

        std::string keyPath;
        if (clientName == "Hydrogen") {
            keyPath = std::format("{}/environments/{}/Library/Application Support/Hydrogen/Key.txt",
                appDataDir, username);
        } else if (clientName == "Delta") {
            keyPath = std::format("{}/environments/{}/Library/Delta/Cache/license",
                appDataDir, username);
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
        			LOG_INFO("Key already up to date for {}", clientName);
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

    LOG_INFO("Key written successfully for {}", clientName);
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

    LOG_INFO("playerPath: {}", clientPath);
    LOG_INFO("protocolURL: {}", protocolURL);
    LOG_INFO("Launching sandboxed client: {}", clientName);
    LOG_INFO("Profile: {}", profileId);
    LOG_INFO("Profile path: {}", profilePath);

    bool mobileClient = isMobileClient2(clientPath);
    LOG_INFO("mobileClient: {}", mobileClient);

    std::string logDir = std::format("{}/Logs", profilePath);
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);

    std::vector<const char*> argv = {"open", "-a", clientPath.c_str()};
    if (!protocolURL.empty()) {
        argv.push_back(protocolURL.c_str());
    }
    argv.push_back(nullptr);

    SpawnOptions opts;
    opts.env = {{"HOME", profilePath}};

    if (mobileClient) {
        opts.env["CFFIXED_USER_HOME"] = profilePath;
        opts.env["XDG_DATA_HOME"] = profilePath + "/Documents";
    }

    for (const auto& [key, value] : opts.env) {
        LOG_INFO("{} = {}", key, value);
    }

    opts.stdoutPath = std::format("{}/roblox_stdout.log", logDir);
    opts.stderrPath = std::format("{}/roblox_stderr.log", logDir);
    opts.waitForCompletion = false;

    bool result = spawnProcessWithEnv("/usr/bin/open", argv, opts);

    if (!result) {
        LOG_ERROR("Failed to launch client");
        return false;
    }

    LOG_INFO("Client launched successfully");
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

}
#endif