#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

struct PublicServerInfo;
struct GameInfo;

namespace Roblox {

struct GameDetail {
    std::string name;
    std::string genre;
    std::string genreL1;
    std::string genreL2;
    std::string description;
    uint64_t visits = 0;
    uint64_t favorites = 0;
    int playing = 0;
    int maxPlayers = 0;
    int priceRobux = -1;
    std::string createdIso;
    std::string updatedIso;

    std::string creatorName;
    uint64_t creatorId = 0;
    std::string creatorType;
    bool creatorVerified = false;
};

struct ServerPage {
    std::vector<PublicServerInfo> data;
    std::string nextCursor;
    std::string prevCursor;
};

struct GamePrivateServerPlayer {
    uint64_t id{};
    std::string name;
    std::string displayName;
};

struct GamePrivateServerInfo {
    std::string serverId;
    std::string name;
    uint64_t vipServerId = 0;
    std::string accessCode;
    int maxPlayers = 0;

    int playing = 0;
    double fps = 0.0;
    int ping = 0;

    std::vector<GamePrivateServerPlayer> players;

    std::string ownerName;
    std::string ownerDisplayName;
    bool ownerVerified = false;
    uint64_t ownerId = 0;
};

struct GamePrivateServersPage {
    std::vector<GamePrivateServerInfo> data;
    std::optional<std::string> nextCursor;
    std::optional<std::string> prevCursor;
    bool gameJoinRestricted{false};
};

struct MyPrivateServerInfo {
    uint64_t privateServerId{};
    uint64_t universeId{};
    uint64_t placeId{};
    uint64_t ownerId{};
    std::string ownerName;
    std::string name;
    std::string universeName;
    std::string expirationDate;
    bool active{};
    bool willRenew{};
    std::optional<int> priceInRobux;
};

struct MyPrivateServersPage {
    std::vector<MyPrivateServerInfo> data;
    std::optional<std::string> nextCursor;
    std::optional<std::string> prevCursor;
};

GameDetail getGameDetail(uint64_t universeId);

ServerPage getPublicServersPage(uint64_t placeId, const std::string& cursor = {});

std::vector<GameInfo> searchGames(const std::string& query);

GamePrivateServersPage getPrivateServersForGame(uint64_t placeId, const std::string& cookie);

MyPrivateServersPage getAllPrivateServers(int serverTab, const std::string& cookie);

}