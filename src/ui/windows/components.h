#pragma once

#include <string>
#include "data.h"
#include "servers/servers.h"

void RenderAccountsTable(std::vector<AccountData> &, const char *, float);

bool RenderMainMenu();

struct PublicServerInfo {
	std::string jobId;
	int currentPlayers = 0;
	int maximumPlayers = 0;
	double averagePing = 0.0;
	double averageFps = 0.0;
};

struct GameInfo {
	std::string name;
	uint64_t universeId = 0;
	uint64_t placeId = 0;
	int playerCount = 0;
	int upVotes = 0;
	int downVotes = 0;
	std::string creatorName;
	bool creatorVerified = false;
};
