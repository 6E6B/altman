#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <sys/types.h>
#endif

namespace MultiInstance {

#ifdef _WIN32
    void Enable();
    void Disable();

#elif __APPLE__

    struct RobloxInstance {
            pid_t pid;
            std::string profileId;
            std::string profilePath;
            std::string clientName;
            time_t launchTime;
    };

    struct PatchTarget {
            std::string patchName;
            std::string patternStr;
            std::vector<uint8_t> patchBytes;
            uint32_t offset {0};
    };

    void Enable();
    void Disable();

    std::string getUserClientPath(const std::string &username, const std::string &clientName);
    std::string getBaseClientPath(const std::string &clientName);
    std::string getClientPath(const std::string &username, const std::string &clientName);

    void saveSourceHash(const std::string &destPath);
    bool needsClientUpdate(const std::string &sourcePath, const std::string &destPath);

    bool cleanupUserEnvironment(const std::string &username);
    bool createProfileEnvironment(const std::string &profileId, std::string &profilePath);

    bool isClientInstalled(const std::string &username, const std::string &clientName);
    bool isBaseClientInstalled(const std::string &clientName);
    const std::vector<std::string> &getAvailableClients(bool forceRefresh = false);
    const std::vector<std::string> &getAvailableClientsForUI(bool refresh = false);

    bool createKeychain(const std::string &profileId);
    bool unlockKeychain(const std::string &profileId);

    bool modifyBundleIdentifier(
        const std::string &username,
        const std::string &clientName,
        const std::string &profileId,
        bool isInitialSetup
    );
    bool needsBundleIdModification(
        const std::string &username,
        const std::string &clientName,
        const std::string &expectedProfileId
    );

    bool ensureClientKey(const std::string &username, const std::string &clientName, const std::string &key);

    bool launchSandboxedClient(
        const std::string &username,
        const std::string &clientName,
        const std::string &profileId,
        const std::string &profilePath,
        const std::string &protocolURL
    );

    bool stopSandboxedRoblox(pid_t pid);

#endif

} // namespace MultiInstance
