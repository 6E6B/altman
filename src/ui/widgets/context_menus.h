#pragma once

#include <cstdint>
#include <string>
#include <functional>

struct StandardJoinMenuParams {
	uint64_t placeId = 0;
	uint64_t universeId = 0;
	std::string jobId;
	bool enableLaunchGame = true;
	bool enableLaunchInstance = true; // only applies if jobId not empty
	std::function<void()> onLaunchGame;      // optional
	std::function<void()> onLaunchInstance;  // optional
	std::function<void()> onFillGame;        // optional
	std::function<void()> onFillInstance;    // optional
};

void RenderStandardJoinMenu(const StandardJoinMenuParams& params);