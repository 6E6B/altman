#pragma once

#include "network/http.hpp"
#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <vector>
#include <utility>

#ifdef _WIN32
    #include <windows.h>
#elif __APPLE__
    #include <unistd.h>
    #include <spawn.h>
    #include <sys/wait.h>
#endif

#include "core/logging.hpp"
#include "ui/notifications.h"
#include "../../components/data.h"
#include "roblox_control.h"

using namespace std;
using namespace std::chrono;

static string urlEncode(const string &s) {
    ostringstream out;
    out << hex << uppercase;
    for (unsigned char c: s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out << c;
        else
            out << '%' << setw(2) << setfill('0') << static_cast<int>(c);
    }
    return out.str();
}

#ifdef _WIN32
inline HANDLE startRoblox(uint64_t placeId, const string &jobId, const string &cookie) {
    LOG_INFO("Fetching x-csrf token");
    auto csrfResponse = HttpClient::post(
        "https://auth.roblox.com/v1/authentication-ticket",
        {{"Cookie", ".ROBLOSECURITY=" + cookie}});

    auto csrfToken = csrfResponse.headers.find("x-csrf-token");
    if (csrfToken == csrfResponse.headers.end()) {
        cerr << "failed to get CSRF token\n";
        LOG_ERROR("Failed to get CSRF token");
        return nullptr;
    }

    LOG_INFO("Fetching authentication ticket");
    auto ticketResponse = HttpClient::post(
        "https://auth.roblox.com/v1/authentication-ticket",
        {
            {"Cookie", ".ROBLOSECURITY=" + cookie},
            {"Origin", "https://www.roblox.com"},
            {"Referer", "https://www.roblox.com/"},
            {"X-CSRF-TOKEN", csrfToken->second}
        });

    auto ticket = ticketResponse.headers.find("rbx-authentication-ticket");
    if (ticket == ticketResponse.headers.end()) {
        cerr << "failed to get authentication ticket\n";
        LOG_ERROR("Failed to get authentication ticket");
        return nullptr;
    }

    auto nowMs = duration_cast<milliseconds>(
                system_clock::now().time_since_epoch())
            .count();
    ostringstream ts;
    ts << nowMs;

    string placeLauncherUrl =
            "https://assetgame.roblox.com/game/PlaceLauncher.ashx?"
            "request=RequestGameJob"
            "&browserTrackerId=147062882894"
            "&placeId=" +
            to_string(placeId) +
            "&gameId=" + jobId +
            "&isPlayTogetherGame=false"
            "+browsertrackerid:147062882894"
            "+robloxLocale:en_us"
            "+gameLocale:en_us"
            "+channel:";

    string protocolLaunchCommand =
            "roblox-player:1+launchmode:play"
            "+gameinfo:" +
            ticket->second +
            "+launchtime:" + ts.str() +
            "+placelauncherurl:" + urlEncode(placeLauncherUrl);

    string logMessage = "Attempting to launch Roblox for place ID: " + to_string(placeId) + (
                            jobId.empty() ? "" : " with Job ID: " + jobId);
    LOG_INFO(logMessage);

    wstring notificationTitle = L"Launching";
    wostringstream notificationMessageStream;
    notificationMessageStream << L"Attempting to launch Roblox for place ID: " << placeId;
    if (!jobId.empty()) {
        notificationMessageStream << L" with Job ID: " << jobId.c_str();
    }
    Notifications::showNotification(notificationTitle.c_str(), notificationMessageStream.str().c_str());

    SHELLEXECUTEINFOA executionInfo{sizeof(executionInfo)};
    executionInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    executionInfo.lpVerb = "open";
    executionInfo.lpFile = protocolLaunchCommand.c_str();
    executionInfo.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExA(&executionInfo)) {
        LOG_ERROR("ShellExecuteExA failed for Roblox launch. Error: " + to_string(GetLastError()));
        cerr << "ShellExecuteEx failed: " << GetLastError() << "\n";
        return nullptr;
    }

    LOG_INFO("Roblox process started successfully for place ID: " + to_string(placeId));
    return executionInfo.hProcess;
}

#elif __APPLE__

inline bool startRoblox(uint64_t placeId, const string &jobId, const string &cookie) {
    LOG_INFO("Fetching x-csrf token");
    auto csrfResponse = HttpClient::post(
        "https://auth.roblox.com/v1/authentication-ticket",
        {{"Cookie", ".ROBLOSECURITY=" + cookie}});

    auto csrfToken = csrfResponse.headers.find("x-csrf-token");
    if (csrfToken == csrfResponse.headers.end()) {
        cerr << "failed to get CSRF token\n";
        LOG_ERROR("Failed to get CSRF token");
        return false;
    }

    LOG_INFO("Fetching authentication ticket");
    auto ticketResponse = HttpClient::post(
        "https://auth.roblox.com/v1/authentication-ticket",
        {
            {"Cookie", ".ROBLOSECURITY=" + cookie},
            {"Origin", "https://www.roblox.com"},
            {"Referer", "https://www.roblox.com/"},
            {"X-CSRF-TOKEN", csrfToken->second}
        });

    auto ticket = ticketResponse.headers.find("rbx-authentication-ticket");
    if (ticket == ticketResponse.headers.end()) {
        cerr << "failed to get authentication ticket\n";
        LOG_ERROR("Failed to get authentication ticket");
        return false;
    }

    auto nowMs = duration_cast<milliseconds>(
                system_clock::now().time_since_epoch())
            .count();
    ostringstream ts;
    ts << nowMs;

    string placeLauncherUrl =
            "https://assetgame.roblox.com/game/PlaceLauncher.ashx?"
            "request=RequestGameJob"
            "&browserTrackerId=147062882894"
            "&placeId=" +
            to_string(placeId) +
            "&gameId=" + jobId +
            "&isPlayTogetherGame=false"
            "+browsertrackerid:147062882894"
            "+robloxLocale:en_us"
            "+gameLocale:en_us"
            "+channel:";

    string protocolLaunchCommand =
            "roblox-player:1+launchmode:play"
            "+gameinfo:" +
            ticket->second +
            "+launchtime:" + ts.str() +
            "+placelauncherurl:" + urlEncode(placeLauncherUrl);

    string logMessage = "Attempting to launch Roblox for place ID: " + to_string(placeId) + (
                            jobId.empty() ? "" : " with Job ID: " + jobId);
    LOG_INFO(logMessage);

    // Use 'open' command on macOS to open the Roblox protocol URL
    string command = "open \"" + protocolLaunchCommand + "\"";
    int result = system(command.c_str());
    
    if (result != 0) {
        LOG_ERROR("Failed to launch Roblox. system() returned: " + to_string(result));
        return false;
    }

    LOG_INFO("Roblox process started successfully for place ID: " + to_string(placeId));
    
    // Sleep briefly to allow Roblox to initialize before next launch
    usleep(500000); // 500ms
    
    return true;
}

#endif

inline void launchRobloxSequential(uint64_t placeId, const std::string &jobId,
                                   const std::vector<std::pair<int, std::string>> &accounts) {
    if (g_killRobloxOnLaunch)
        RobloxControl::KillRobloxProcesses();

    if (g_clearCacheOnLaunch)
        RobloxControl::ClearRobloxCache();

    for (const auto &[accountId, cookie]: accounts) {
        LOG_INFO("Launching Roblox for account ID: " + std::to_string(accountId) +
            " PlaceID: " + std::to_string(placeId) +
            (jobId.empty() ? "" : " JobID: " + jobId));
        
#ifdef _WIN32
        HANDLE proc = startRoblox(placeId, jobId, cookie);
        if (proc) {
            WaitForInputIdle(proc, INFINITE);
            CloseHandle(proc);
            LOG_INFO("Roblox launched successfully for account ID: " +
                std::to_string(accountId));
        } else {
            LOG_ERROR("Failed to start Roblox for account ID: " +
                std::to_string(accountId));
        }
#elif __APPLE__
        bool success = startRoblox(placeId, jobId, cookie);
        if (success) {
            LOG_INFO("Roblox launched successfully for account ID: " +
                std::to_string(accountId));
        } else {
            LOG_ERROR("Failed to start Roblox for account ID: " +
                std::to_string(accountId));
        }
#endif
    }
}