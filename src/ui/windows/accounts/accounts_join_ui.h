#pragma once
#include <string>

enum JoinType {
	PrivateServer = 0,
	Game = 1,
	GameServer = 2,
	User = 3
};
void RenderJoinOptions();

void FillJoinOptions(uint64_t placeId, const std::string &jobId);
