#include "ipa_installer.h"

#include <filesystem>
#include <fstream>
#include <format>
#include <thread>
#include <array>
#include <regex>
#include <algorithm>
#include <cctype>
#include <vector>
#include <sys/sysctl.h>

#include "console/console.h"
#include "http.h"

namespace IPAInstaller {

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

std::string GetHardwareArchitecture() {
    int isArm = 0;
    size_t size = sizeof(isArm);

    if (sysctlbyname("hw.optional.arm64", &isArm, &size, nullptr, 0) == 0 && isArm) {
        return "aarch64";
    }
    return "x86_64";
}

bool DownloadPackage(const std::string& appDataDir, const std::string& url,
                     const std::string& client, ProgressCallback progressCb) {
    const std::filesystem::path clientsDir = std::filesystem::path(appDataDir) / "clients";

    std::error_code ec;
    std::filesystem::create_directories(clientsDir, ec);
    if (ec) {
        LOG_ERROR("Failed to create clients directory: {}", ec.message());
        return false;
    }

    const std::filesystem::path clientPath = clientsDir / (client + ".ipa");

    LOG_INFO("Downloading {} from {}", client, url);

    auto progress_adapter = [progressCb](size_t downloaded, size_t total) {
        if (progressCb && total > 0) {
            const float percent = static_cast<float>(downloaded) / static_cast<float>(total);
            progressCb(percent, std::format("Downloaded {} / {} bytes", downloaded, total));
        }
    };

    return HttpClient::download(
        url,
        clientPath.string(),
        {{"User-Agent", std::format("RaptorManager/1.0")}},
        progress_adapter
    );
}

bool RestructureAppBundle(const std::filesystem::path& appDir, ProgressCallback progressCb) {
    if (progressCb) {
        progressCb(0.0f, "Restructuring app bundle...");
    }

    std::error_code ec;

    const std::filesystem::path contentsDir = appDir / "Contents";
    const std::filesystem::path macosDir = contentsDir / "MacOS";
    const std::filesystem::path frameworksDir = contentsDir / "Frameworks";
    const std::filesystem::path resourcesDir = contentsDir / "Resources";

    std::filesystem::create_directories(macosDir, ec);
    if (ec) {
        LOG_ERROR("Failed to create MacOS directory: {}", ec.message());
        return false;
    }

    std::filesystem::create_directories(frameworksDir, ec);
    if (ec) {
        LOG_ERROR("Failed to create Frameworks directory: {}", ec.message());
        return false;
    }

    std::filesystem::create_directories(resourcesDir, ec);
    if (ec) {
        LOG_ERROR("Failed to create Resources directory: {}", ec.message());
        return false;
    }

    const std::filesystem::path robloxExec = appDir / "Roblox";
    if (std::filesystem::exists(robloxExec)) {
        std::filesystem::rename(robloxExec, macosDir / "Roblox", ec);
        if (ec) {
            LOG_ERROR("Failed to move Roblox executable: {}", ec.message());
            return false;
        }
    } else {
        LOG_ERROR("Roblox executable not found");
        return false;
    }

    const std::filesystem::path oldFrameworks = appDir / "Frameworks";
    if (std::filesystem::exists(oldFrameworks)) {
        for (const auto& entry : std::filesystem::directory_iterator(oldFrameworks)) {
            std::filesystem::rename(entry.path(), frameworksDir / entry.path().filename(), ec);
            if (ec) {
                LOG_ERROR("Failed to move {}: {}", entry.path().filename().string(), ec.message());
                return false;
            }
        }
        std::filesystem::remove_all(oldFrameworks, ec);
    }

    const std::filesystem::path oldPlist = appDir / "Info.plist";
    if (std::filesystem::exists(oldPlist)) {
        std::filesystem::rename(oldPlist, contentsDir / "Info.plist", ec);
        if (ec) {
            LOG_ERROR("Failed to move Info.plist: {}", ec.message());
            return false;
        }
    } else {
        LOG_ERROR("Info.plist not found");
        return false;
    }

    for (const auto& entry : std::filesystem::directory_iterator(appDir)) {
        if (entry.path().filename() == "Contents") {
            continue;
        }

        std::filesystem::rename(entry.path(), resourcesDir / entry.path().filename(), ec);
        if (ec) {
            LOG_ERROR("Failed to move {} to Resources: {}", entry.path().filename().string(), ec.message());
            return false;
        }
    }

    if (progressCb) {
        progressCb(1.0f, "App bundle restructured");
    }

    LOG_INFO("Successfully restructured app bundle to macOS format");
    return true;
}

bool InstallPackage(const std::string& appDataDir, const std::string& client,
                    ProgressCallback progressCb) {
    const std::filesystem::path clientsDir = std::filesystem::path(appDataDir) / "clients";
    const std::filesystem::path ipaPath = clientsDir / (client + ".ipa");

    if (!std::filesystem::exists(ipaPath)) {
        LOG_ERROR("Package {} not found. Anti-virus might be blocking.", client);
        return false;
    }

    if (progressCb) {
        progressCb(0.0f, "Extracting IPA...");
    }

    const std::string unzipCmd = std::format(
        "/usr/bin/unzip -o -q -d \"{}\" \"{}\"",
        clientsDir.string(),
        ipaPath.string()
    );

    std::string output;
    if (!ExecuteCommand(unzipCmd, output)) {
        LOG_ERROR("Failed to unzip {}: {}", client, output);
        return false;
    }

    const std::filesystem::path appDir = clientsDir / "Payload" / "Roblox.app";
    const std::filesystem::path finalAppPath = clientsDir / (client + ".app");

    std::error_code ec;
    std::filesystem::rename(appDir, finalAppPath, ec);
    if (ec) {
        LOG_ERROR("Failed to rename app: {}", ec.message());
        return false;
    }

    std::filesystem::remove_all(clientsDir / "Payload", ec);
    if (ec) {
        LOG_WARN("Failed to remove Payload dir: {}", ec.message());
    }

    if (progressCb) {
        progressCb(0.5f, "IPA extracted");
    }

    if (!RestructureAppBundle(finalAppPath, nullptr)) {
        LOG_ERROR("Failed to restructure app bundle");
        return false;
    }

    const std::string chmodCmd = std::format(
        "/bin/chmod +x \"{}\"",
        (finalAppPath / "Contents" / "MacOS" / "Roblox").string()
    );

    if (!ExecuteCommand(chmodCmd, output)) {
        LOG_ERROR("Failed to chmod: {}", output);
        return false;
    }

    std::filesystem::remove(ipaPath, ec);
    if (ec) {
        LOG_WARN("Failed to remove IPA: {}", ec.message());
    }

    if (progressCb) {
        progressCb(1.0f, "Package installed");
    }

    return true;
}

bool Codesign(const std::filesystem::path& path, bool sign,
              ProgressCallback progressCb) {
    if (progressCb) {
        progressCb(0.0f, sign ? "Signing..." : "Removing signature...");
    }

    const std::string command = sign
        ? std::format("/usr/bin/codesign -s - \"{}\"", path.string())
        : std::format("/usr/bin/codesign --remove-signature \"{}\"", path.string());

    std::string output;
    if (!ExecuteCommand(command, output)) {
        LOG_ERROR("Codesign failed: {}", output);
        return false;
    }

    if (progressCb) {
        progressCb(1.0f, sign ? "Signed" : "Signature removed");
    }

    return true;
}

bool FixLibraryPaths(const std::filesystem::path& executablePath,
                     const std::filesystem::path& frameworksDir,
                     ProgressCallback progressCb) {
    if (progressCb) {
        progressCb(0.0f, "Fixing library paths...");
    }

    std::string output;

    const std::filesystem::path libgloopPath = frameworksDir / "libgloop.dylib";
    if (std::filesystem::exists(libgloopPath)) {
        const std::string changeIdCmd = std::format(
            "/usr/bin/install_name_tool -id \"@rpath/libgloop.dylib\" \"{}\"",
            libgloopPath.string()
        );

        if (!ExecuteCommand(changeIdCmd, output)) {
            LOG_ERROR("Failed to change libgloop.dylib id: {}", output);
            return false;
        }

        const std::string changeRefCmd = std::format(
            "/usr/bin/install_name_tool -change \"@executable_path/Frameworks/libgloop.dylib\" \"@rpath/libgloop.dylib\" \"{}\"",
            executablePath.string()
        );

        if (!ExecuteCommand(changeRefCmd, output)) {
            LOG_ERROR("Failed to change libgloop reference in executable: {}", output);
            return false;
        }

        LOG_INFO("Successfully fixed libgloop.dylib paths");
    }

    if (progressCb) {
        progressCb(1.0f, "Library paths fixed");
    }

    return true;
}

bool FixRpath(const std::filesystem::path& executablePath, ProgressCallback progressCb) {
    if (progressCb) {
        progressCb(0.0f, "Fixing rpath...");
    }

    const std::vector<std::string> oldRpaths = {
        "@executable_path/Frameworks",
        "@loader_path/Frameworks"
    };

    std::string output;
    for (const auto& oldRpath : oldRpaths) {
        const std::string deleteCmd = std::format(
            "/usr/bin/install_name_tool -delete_rpath \"{}\" \"{}\" 2>&1 || true",
            oldRpath,
            executablePath.string()
        );
        ExecuteCommand(deleteCmd, output);
    }

    const std::string addCmd = std::format(
        "/usr/bin/install_name_tool -add_rpath \"@executable_path/../Frameworks\" \"{}\"",
        executablePath.string()
    );

    if (!ExecuteCommand(addCmd, output)) {
        LOG_ERROR("Failed to add rpath: {}", output);
        return false;
    }

    if (progressCb) {
        progressCb(1.0f, "Rpath fixed");
    }

    LOG_INFO("Successfully fixed rpath");
    return true;
}

bool Convert(const std::filesystem::path& path, const std::string& name,
             ProgressCallback progressCb) {
    if (progressCb) {
        progressCb(0.0f, std::format("Converting {}...", name));
    }

    const std::string command = std::format(
        "/usr/bin/vtool -set-build-version maccatalyst 13.0 18.2 -replace -output \"{}\" \"{}\"",
        path.string(),
        path.string()
    );

    std::string output;
    if (!ExecuteCommand(command, output)) {
        LOG_ERROR("Convert failed: {}", output);
        return false;
    }

    if (progressCb) {
        progressCb(1.0f, std::format("{} converted", name));
    }

    return true;
}

bool ConvertPlist(const std::filesystem::path& path, ProgressCallback progressCb) {
    if (progressCb) {
        progressCb(0.0f, "Converting plist...");
    }

    const std::string command = std::format(
        "/usr/bin/plutil -convert xml1 -o \"{}\" \"{}\"",
        path.string(),
        path.string()
    );

    std::string output;
    if (!ExecuteCommand(command, output)) {
        LOG_ERROR("Convert plist failed: {}", output);
        return false;
    }

    if (progressCb) {
        progressCb(1.0f, "Plist converted");
    }

    return true;
}

bool ModifyPlist(const std::filesystem::path& plistPath, ProgressCallback progressCb) {
    if (progressCb) {
        progressCb(0.0f, "Modifying plist...");
    }

    std::ifstream inFile(plistPath);
    if (!inFile) {
        LOG_ERROR("Failed to open Info.plist for reading");
        return false;
    }

    std::string contents((std::istreambuf_iterator<char>(inFile)),
                         std::istreambuf_iterator<char>());
    inFile.close();

    // Replace bundle identifier
    size_t pos = contents.find("com.gloop.deltamobile");
    while (pos != std::string::npos) {
        contents.replace(pos, 21, "com.roblox.RobloxPlayer");
        pos = contents.find("com.gloop.deltamobile", pos + 23);
    }

    // Remove iOS-specific keys
    const std::vector<std::string> keysToRemove = {
        "LSRequiresIPhoneOS",
        "UIDeviceFamily",
        "CFBundleSupportedPlatforms",
        "UISupportedInterfaceOrientations",
        "UISupportedInterfaceOrientations~ipad",
        "UIRequiresFullScreen",
        "UIStatusBarHidden",
        "UIStatusBarHidden~ipad",
        "UIStatusBarStyle",
        "MinimumOSVersion",
        "DTPlatformName",
        "UIViewControllerBasedStatusBarAppearance",
        "UIPrerenderedIcon",
        "UIRequiredDeviceCapabilities",
        "UILaunchStoryboardName"
    };

    for (const auto& key : keysToRemove) {
        std::regex keyRegex("<key>" + key + "</key>\\s*<[^>]+>.*?</[^>]+>");
        contents = std::regex_replace(contents, keyRegex, "");

        std::regex keyRegexMultiline("<key>" + key + "</key>\\s*<(array|dict)>.*?</\\1>");
        contents = std::regex_replace(contents, keyRegexMultiline, "");
    }

    const std::string macOSKeys = R"(	<key>LSMinimumSystemVersion</key>
	<string>11.0</string>
	<key>NSHighResolutionCapable</key>
	<true/>
	<key>UIApplicationSupportsIndirectInputEvents</key>
	<true/>
	<key>GCSupportsControllerUserInteraction</key>
	<true/>
	<key>UISupportsTouchBar</key>
	<false/>
)";

    size_t closingDict = contents.rfind("</dict>");
    if (closingDict != std::string::npos) {
        contents.insert(closingDict, macOSKeys);
    }

    std::ofstream outFile(plistPath);
    if (!outFile) {
        LOG_ERROR("Failed to open Info.plist for writing");
        return false;
    }
    outFile << contents;
    outFile.close();

    if (progressCb) {
        progressCb(1.0f, "Plist modified");
    }

    LOG_INFO("Successfully modified Info.plist for macOS");
    return true;
}

bool InstallIPA(const std::string& appDataDir, const std::string& client,
                const std::string& version, ProgressCallback progressCb) {
    const std::string arch = GetHardwareArchitecture();
    if (arch != "aarch64") {
        LOG_ERROR("IPA installation only available on Apple Silicon");
        return false;
    }

    if (client == "Delta") {
        if (progressCb) progressCb(0.0f, "Downloading IPA...");

        const std::string url = std::format("https://cdn.gloopup.net/file/Delta-{}.ipa", version);

        auto downloadProgress = [progressCb](float p, const std::string& msg) {
            if (progressCb) progressCb(0.0f + p * 0.3f, msg);
        };

        if (!DownloadPackage(appDataDir, url, client, downloadProgress)) {
            LOG_ERROR("Failed to download package");
            return false;
        }
    }

    if (progressCb) progressCb(0.3f, "Installing package...");

    auto installProgress = [progressCb](float p, const std::string& msg) {
        if (progressCb) progressCb(0.3f + p * 0.1f, msg);
    };

    if (!InstallPackage(appDataDir, client, installProgress)) {
        LOG_ERROR("Failed to install package");
        return false;
    }

    const std::filesystem::path clientsDir = std::filesystem::path(appDataDir) / "clients";
    const std::filesystem::path robloxDir = clientsDir / (client + ".app");

    if (!std::filesystem::exists(robloxDir)) {
        LOG_ERROR("Package {} not found after install", client);
        return false;
    }

    const std::filesystem::path personaDir = robloxDir / "Contents" / "Frameworks" / "Persona2.framework";
    const std::filesystem::path robloxlibDir = robloxDir / "Contents" / "Frameworks" / "RobloxLib.framework";
    const std::filesystem::path libgloopPath = robloxDir / "Contents" / "Frameworks" / "libgloop.dylib";
    const std::filesystem::path robloxExec = robloxDir / "Contents" / "MacOS" / "Roblox";
    const std::filesystem::path frameworksDir = robloxDir / "Contents" / "Frameworks";

    if (!std::filesystem::exists(personaDir)) {
        LOG_ERROR("Persona2.framework not found");
        return false;
    }

    if (!std::filesystem::exists(robloxlibDir)) {
        LOG_ERROR("RobloxLib.framework not found");
        return false;
    }

    if (progressCb) progressCb(0.4f, "Removing signatures...");

    if (std::filesystem::exists(libgloopPath)) {
        if (!Codesign(libgloopPath, false, nullptr)) {
            return false;
        }
    }

    if (!Codesign(personaDir, false, nullptr)) {
        return false;
    }

    if (!Codesign(robloxlibDir, false, nullptr)) {
        return false;
    }

    if (!Codesign(robloxDir, false, nullptr)) {
        return false;
    }

    if (progressCb) progressCb(0.5f, "Converting binaries...");

    auto convertProgress1 = [progressCb](float p, const std::string& msg) {
        if (progressCb) progressCb(0.5f + p * 0.05f, msg);
    };
    if (!Convert(robloxExec, "Roblox executable", convertProgress1)) {
        return false;
    }

    if (progressCb) progressCb(0.55f, "Fixing paths...");

    auto rpathProgress = [progressCb](float p, const std::string& msg) {
        if (progressCb) progressCb(0.55f + p * 0.05f, msg);
    };
    if (!FixRpath(robloxExec, rpathProgress)) {
        return false;
    }

    auto libProgress = [progressCb](float p, const std::string& msg) {
        if (progressCb) progressCb(0.6f + p * 0.05f, msg);
    };
    if (!FixLibraryPaths(robloxExec, frameworksDir, libProgress)) {
        return false;
    }

    auto convertProgress2 = [progressCb](float p, const std::string& msg) {
        if (progressCb) progressCb(0.65f + p * 0.05f, msg);
    };
    if (!Convert(personaDir / "Persona2", "Persona2", convertProgress2)) {
        return false;
    }

    auto convertProgress3 = [progressCb](float p, const std::string& msg) {
        if (progressCb) progressCb(0.7f + p * 0.05f, msg);
    };
    if (!Convert(robloxlibDir / "RobloxLib", "RobloxLib", convertProgress3)) {
        return false;
    }

    if (std::filesystem::exists(libgloopPath)) {
        if (!Convert(libgloopPath, "libgloop", nullptr)) {
            return false;
        }
    }

    if (progressCb) progressCb(0.75f, "Modifying plist...");

    const std::filesystem::path plistPath = robloxDir / "Contents" / "Info.plist";

    auto plistConvertProgress = [progressCb](float p, const std::string& msg) {
        if (progressCb) progressCb(0.75f + p * 0.025f, msg);
    };
    if (!ConvertPlist(plistPath, plistConvertProgress)) {
        return false;
    }

    auto plistModifyProgress = [progressCb](float p, const std::string& msg) {
        if (progressCb) progressCb(0.775f + p * 0.025f, msg);
    };
    if (!ModifyPlist(plistPath, plistModifyProgress)) {
        return false;
    }

    if (progressCb) progressCb(0.8f, "Signing binaries...");

    if (std::filesystem::exists(libgloopPath)) {
        if (!Codesign(libgloopPath, true, nullptr)) {
            return false;
        }
    }

    if (progressCb) progressCb(0.85f, "Signing frameworks...");

    if (!Codesign(personaDir, true, nullptr)) {
        return false;
    }

    if (!Codesign(robloxlibDir, true, nullptr)) {
        return false;
    }

    if (progressCb) progressCb(0.95f, "Signing app bundle...");

    if (!Codesign(robloxDir, true, nullptr)) {
        return false;
    }

    if (progressCb) progressCb(1.0f, "Installation complete!");

    LOG_INFO("Successfully installed {} IPA", client);
    return true;
}

void InstallIPAAsync(const std::string& appDataDir, const std::string& client,
                     const std::string& version, ProgressCallback progressCb,
                     CompletionCallback completionCb) {
    std::thread([appDataDir, client, version, progressCb, completionCb]() {
        const bool success = InstallIPA(appDataDir, client, version, progressCb);
        
        if (completionCb) {
            completionCb(success, success ? "Installation successful" : "Installation failed");
        }
    }).detach();
}

}