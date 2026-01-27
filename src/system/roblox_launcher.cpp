#include "roblox_launcher.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <random>
#include <regex>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#elif __APPLE__
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "utils/paths.h"
#endif

#include "components/data.h"
#include "console/console.h"
#include "multi_instance.h"
#include "network/http.h"
#include "network/roblox/auth.h"
#include "network/roblox/common.h"
#include "network/roblox/games.h"
#include "roblox_control.h"
#include "ui/widgets/notifications.h"
#include "utils/account_utils.h"
#include "utils/thread_task.h"

LaunchParams LaunchParams::standard(uint64_t placeId) {
    return {LaunchMode::Job, placeId, ""};
}

LaunchParams LaunchParams::gameJob(uint64_t placeId, const std::string &jobId) {
    return {LaunchMode::GameJob, placeId, jobId};
}

LaunchParams LaunchParams::privateServer(const std::string &shareLink) {
    return {LaunchMode::PrivateServer, 0, shareLink};
}

LaunchParams LaunchParams::privateServerDirect(uint64_t placeId, const std::string &accessCode) {
    return {LaunchMode::PrivateServerDirect, placeId, accessCode};
}

LaunchParams LaunchParams::followUser(const std::string &userId) {
    return {LaunchMode::FollowUser, 0, userId};
}

std::string urlEncode(const std::string &s) {
    std::ostringstream out;
    out << std::hex << std::uppercase;
    for (unsigned char c: s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return out.str();
}

std::string generateBrowserTrackerId() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis1(100000, 175000);
    std::uniform_int_distribution<> dis2(100000, 900000);
    return std::format("{}{}", dis1(gen), dis2(gen));
}

std::string getCurrentTimestampMs() {
    auto nowMs
        = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
              .count();
    return std::to_string(nowMs);
}

struct LaunchUrls {
        std::string desktop;
        std::string mobile;
        uint64_t resolvedPlaceId;
};

bool resolvePrivateServer(
    const std::string &link,
    const std::string &cookie,
    uint64_t &placeId,
    std::string &linkCode,
    std::string &accessCode
) {
    std::smatch match;

    std::regex shareLinkRegex(R"(roblox\.com/share\?code=([^&]+)&type=Server)");
    std::regex directLinkRegex(R"(roblox\.com/games/(\d+)[^?]*\?privateServerLinkCode=([0-9]+))");

    bool isShareLink = std::regex_search(link, match, shareLinkRegex);
    bool isDirectLink = !isShareLink && std::regex_search(link, match, directLinkRegex);

    if (!isShareLink && !isDirectLink) {
        LOG_ERROR("Invalid private server link.");
        return false;
    }

    if (isShareLink) {
        std::string shareCode = match[1];

        std::string apiUrl = "https://apis.roblox.com/sharelinks/v1/resolve-link";
        const std::string jsonBody = std::format(R"({{"linkId":"{}","linkType":"Server"}})", shareCode);

        auto apiResponse = Roblox::authenticatedPost(
            apiUrl,
            cookie,
            jsonBody,
            {{"Content-Type", "application/json;charset=UTF-8"}}
        );

        if (apiResponse.status_code != 200) {
            LOG_ERROR("Share resolve failed: HTTP {}", apiResponse.status_code);
            return false;
        }

        try {
            auto jsonResponse = HttpClient::decode(apiResponse);

            if (jsonResponse.contains("status") && jsonResponse["status"].is_string()) {
                const auto &status = jsonResponse["status"].get<std::string>();
                if (status == "Expired") {
                    LOG_ERROR("This private server link is no longer valid.");
                    return false;
                }
            }

            if (!jsonResponse.contains("privateServerInviteData")) {
                LOG_ERROR("Missing invite data.");
                return false;
            }

            auto invite = jsonResponse["privateServerInviteData"];
            placeId = invite["placeId"].get<uint64_t>();
            linkCode = invite["linkCode"].get<std::string>();

        } catch (std::exception &e) {
            LOG_ERROR("JSON parse error: {}", e.what());
            return false;
        }
    }

    if (isDirectLink) {
        placeId = std::stoull(match[1].str());
        linkCode = match[2].str();
    }

    std::string gameUrl = std::format("https://www.roblox.com/games/{}/?privateServerLinkCode={}", placeId, linkCode);

    auto pageResponse = HttpClient::rateLimitedGet(
        gameUrl,
        {
            {"Cookie",     ".ROBLOSECURITY=" + cookie},
            {"User-Agent", "Mozilla/5.0"             }
        }
    );

    std::regex accessCodeRegex(R"(Roblox\.GameLauncher\.joinPrivateGame\(\d+,\s*'([a-f0-9\-]+)',\s*'(\d+)')");

    std::smatch accessMatch;
    if (std::regex_search(pageResponse.text, accessMatch, accessCodeRegex) && accessMatch.size() == 3) {
        accessCode = accessMatch[1].str();
    } else {
        LOG_ERROR("This private server link is no longer valid.");
        return false;
    }

    return true;
}

std::optional<LaunchUrls> buildLaunchUrls(
    const LaunchParams &params,
    const std::string &browserTrackerId,
    const std::string &cookie
) {
    LaunchUrls urls;
    urls.resolvedPlaceId = params.placeId;
    const auto placeIdStr = std::to_string(params.placeId);

    switch (params.mode) {
        case LaunchMode::PrivateServer: {
            std::string linkCode, accessCode;
            uint64_t placeId = params.placeId;

            if (!resolvePrivateServer(params.value, cookie, placeId, linkCode, accessCode)) {
                return std::nullopt;
            }

            urls.resolvedPlaceId = placeId;
            const auto resolvedPlaceIdStr = std::to_string(placeId);

            urls.desktop = std::format(
                "https://assetgame.roblox.com/game/PlaceLauncher.ashx?"
                "request=RequestPrivateGame&placeId={}&accessCode={}&linkCode={}",
                resolvedPlaceIdStr,
                accessCode,
                linkCode
            );
            urls.mobile = std::format("placeId={}&accessCode={}&linkCode={}", resolvedPlaceIdStr, accessCode, linkCode);

            break;
        }

        case LaunchMode::PrivateServerDirect:
            urls.desktop = std::format(
                "https://assetgame.roblox.com/game/PlaceLauncher.ashx?"
                "request=RequestPrivateGame&placeId={}&accessCode={}",
                placeIdStr,
                params.value
            );
            urls.mobile = std::format("placeId={}&accessCode={}", placeIdStr, params.value);
            break;

        case LaunchMode::FollowUser:
            urls.desktop = std::format(
                "https://assetgame.roblox.com/game/PlaceLauncher.ashx?"
                "request=RequestFollowUser&userId={}",
                params.value
            );
            urls.mobile = std::format("userId={}", params.value);
            break;

        case LaunchMode::GameJob:
            urls.desktop = std::format(
                "https://assetgame.roblox.com/game/PlaceLauncher.ashx?"
                "request=RequestGameJob&browserTrackerId={}&placeId={}&gameId={}"
                "&isPlayTogetherGame=false&isTeleport=true",
                browserTrackerId,
                placeIdStr,
                params.value
            );
            urls.mobile = std::format(
                "placeId={}&gameId={}&isPlayTogetherGame=false&isTeleport=true",
                placeIdStr,
                params.value
            );
            break;

        case LaunchMode::Job:
        default:
            urls.desktop = std::format(
                "https://assetgame.roblox.com/game/PlaceLauncher.ashx?"
                "request=RequestGame&browserTrackerId={}&placeId={}&isPlayTogetherGame=false",
                browserTrackerId,
                placeIdStr
            );
            urls.mobile = std::format("placeId={}&isPlayTogetherGame=false", placeIdStr);
            break;
    }

    return urls;
}

std::string buildProtocolCommand(
    bool isMobile,
    const std::string &ticket,
    const std::string &timestamp,
    const std::string &launchUrl,
    const std::string &browserTrackerId
) {
    if (isMobile) {
        return std::format("roblox://{}", launchUrl);
    }

    return std::format(
        "roblox-player:1+launchmode:play+gameinfo:{}+launchtime:{}+"
        "placelauncherurl:{}+browsertrackerid:{}+robloxLocale:en_us+"
        "gameLocale:en_us+channel:+LaunchExp:InApp",
        ticket,
        timestamp,
        urlEncode(launchUrl),
        browserTrackerId
    );
}

#ifdef _WIN32

bool startRoblox(const LaunchParams &params, AccountData acc) {
    auto ticket = Roblox::fetchAuthTicket(acc.cookie);
    if (ticket.empty()) {
        LOG_ERROR("Failed to get authentication ticket");
        return false;
    }

    const auto browserTrackerId = generateBrowserTrackerId();
    const auto timestamp = getCurrentTimestampMs();

    auto urls = buildLaunchUrls(params, browserTrackerId, acc.cookie);
    if (!urls) {
        return false;
    }

    if (acc.username.empty()) {
        LOG_ERROR("Username is empty or invalid");
        return false;
    }

    const auto &launchUrl = urls->desktop;
    const auto protocolCommand = buildProtocolCommand(false, ticket, timestamp, launchUrl, browserTrackerId);

    SHELLEXECUTEINFOA executionInfo {sizeof(executionInfo)};
    executionInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    executionInfo.lpVerb = "open";
    executionInfo.lpFile = protocolCommand.c_str();
    executionInfo.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExA(&executionInfo)) {
        LOG_ERROR("ShellExecuteExA failed for Roblox launch. Error: {}", GetLastError());
        return false;
    }

    if (executionInfo.hProcess) {
        CloseHandle(executionInfo.hProcess);
    }

    LOG_INFO("Roblox launched for account: {}", acc.username);
    return true;
}

#elif __APPLE__

bool startRoblox(const LaunchParams &params, AccountData acc) {
    auto ticket = Roblox::fetchAuthTicket(acc.cookie);
    if (ticket.empty()) {
        LOG_ERROR("Failed to get authentication ticket");
        return false;
    }

    const auto browserTrackerId = generateBrowserTrackerId();
    const auto timestamp = getCurrentTimestampMs();

    auto urls = buildLaunchUrls(params, browserTrackerId, acc.cookie);
    if (!urls) {
        return false;
    }

    const bool isMobile = MultiInstance::isMobileClient(acc.customClientBase);
    const auto launchUrl = isMobile ? urls->mobile : urls->desktop;
    const auto protocolCommand = buildProtocolCommand(isMobile, ticket, timestamp, launchUrl, browserTrackerId);

    if (acc.username.empty()) {
        LOG_ERROR("Username is empty or invalid");
        return false;
    }

    const auto clientName = std::format("Roblox_{}", acc.username);
    if (acc.clientName != clientName) {
        acc.clientName = clientName;
        acc.isUsingCustomClient = true;
        Data::SaveAccounts();
    }

    if (!MultiInstance::createSandboxedRoblox(acc, protocolCommand)) {
        LOG_ERROR("Failed to create sandboxed client instance");
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    return true;
}
#endif // APPLE

void launchWithAccounts(const LaunchParams &params, const std::vector<AccountData> &accounts) {
    if (g_killRobloxOnLaunch) {
        RobloxControl::KillRobloxProcesses();
    }
    if (g_clearCacheOnLaunch) {
        RobloxControl::ClearRobloxCache();
    }

    for (const AccountData acc: accounts) {
        const bool success = startRoblox(params, acc);

        if (success) {
            LOG_INFO("Roblox launched for account ID: {}", acc.id);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        } else {
            LOG_ERROR("Failed to start Roblox for account ID: {}", acc.id);
        }
    }
}

void launchWithSelectedAccounts(LaunchParams params) {
    auto accountPtrs = getUsableSelectedAccounts();
    if (accountPtrs.empty()) {
        return;
    }

    // Copy for thread safety
    std::vector<AccountData> accounts;
    accounts.reserve(accountPtrs.size());
    for (AccountData *acc: accountPtrs) {
        accounts.push_back(*acc);
    }

    ThreadTask::fireAndForget([params = std::move(params), accounts = std::move(accounts)]() {
        launchWithAccounts(params, accounts);
    });
}
