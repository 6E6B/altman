#pragma once

#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include <ctime>

#ifdef __APPLE__
#include <sys/types.h>
#endif

namespace MultiInstance {

#ifdef _WIN32
    // Windows-specific: Simple mutex management
    void Enable();
    void Disable();

#elif __APPLE__
    // macOS-specific: Complex multi-instance management

    struct RobloxInstance {
        pid_t pid;
        std::string profileId;
        std::string profilePath;
        std::string clientName;
        time_t launchTime;
    };

    // Semaphore management
    void Enable();
    void Disable();

    // System utilities
    std::pair<int, std::string> executeCommand(const std::string& cmd);
    std::string getAppDataDirectory();

    // Client path management
    std::string getUserClientPath(const std::string& username, const std::string& clientName);
    std::string getBaseClientPath(const std::string& clientName);
    std::string getClientPath(const std::string& username, const std::string& clientName);

    // Client update management
    void saveSourceHash(const std::string& destPath);
    bool needsClientUpdate(const std::string& sourcePath, const std::string& destPath);

    // Environment management
    bool cleanupUserEnvironment(const std::string& username);
    bool createProfileEnvironment(const std::string& profileId, std::string& profilePath);

    // Client installation checks
    bool isClientInstalled(const std::string& username, const std::string& clientName);
    bool isBaseClientInstalled(const std::string& clientName);
    const std::vector<std::string>& getAvailableClients(bool forceRefresh = false);
    const std::vector<std::string>& getAvailableClientsForUI(bool refresh = false);

    // Keychain management
    bool createKeychain(const std::string& profileId);
    bool unlockKeychain(const std::string& profileId);

    // Bundle identifier modification
    bool modifyBundleIdentifier(
        const std::string& username,
        const std::string& clientName,
        const std::string& profileId,
        bool isInitialSetup
    );
    bool needsBundleIdModification(
        const std::string& username,
        const std::string& clientName,
        const std::string& expectedProfileId
    );

    // Client key management
    bool ensureClientKey(
        const std::string& username,
        const std::string& clientName,
        const std::string& key
    );

    // Client launching
    bool launchSandboxedClient(
        const std::string& username,
        const std::string& clientName,
        const std::string& profileId,
        const std::string& profilePath,
        const std::string& protocolURL
    );

    // Process management
    bool stopSandboxedRoblox(pid_t pid);

#endif

} // namespace MultiInstance