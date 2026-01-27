#pragma once

#include <functional>
#include <optional>
#include <string>

namespace ClientManager {

    using ProgressCallback = std::function<void(float progress, const std::string &message)>;
    using CompletionCallback = std::function<void(bool success, const std::string &message)>;

    struct MacsploitVersion {
            std::string clientVersionUpload;
            std::string appVersion;
            std::string clientVersion;
            std::string relVersion;
            std::string channel;
            std::string changelog;
    };

    struct GlobalVersion {
            std::string globallogs;
    };

    struct PlatformVersion {
            std::optional<std::string> product;
            std::optional<std::string> exploit_version;
            std::optional<std::string> roblox_version;
            std::optional<std::string> changelog;
    };

    struct HydrogenVersion {
            GlobalVersion global;
            PlatformVersion windows;
            PlatformVersion macos;
            PlatformVersion ios;
            PlatformVersion android;
    };

    std::string GetLatestRobloxVersion();
    MacsploitVersion GetMacsploitVersion();
    HydrogenVersion GetHydrogenVersion();
    std::string GetDeltaVersion();

    bool DownloadRoblox(const std::string &version, const std::string &outputPath, ProgressCallback progressCb);
    bool ExtractRoblox(const std::string &zipPath, const std::string &extractTo, ProgressCallback progressCb);
    bool CleanupRobloxApp(const std::string &clientsDir, ProgressCallback progressCb);
    bool DownloadInsertDylib(const std::string &outputPath, ProgressCallback progressCb);
    bool DownloadDylib(const std::string &clientName, const std::string &outputPath, ProgressCallback progressCb);
    bool InsertDylib(
        const std::string &insertDylibPath,
        const std::string &dylibPath,
        const std::string &binaryPath,
        ProgressCallback progressCb
    );
    bool CodeSign(const std::string &appPath, bool remove, ProgressCallback progressCb);

    void InstallClientAsync(
        const std::string &clientName,
        ProgressCallback progressCb,
        CompletionCallback completionCb
    );
    void RemoveClientAsync(const std::string &clientName, CompletionCallback completionCb);

} // namespace ClientManager
