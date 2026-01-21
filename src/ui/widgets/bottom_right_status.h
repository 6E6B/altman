#pragma once

#include <string>

namespace ButtonRightStatus {
	void Set(std::string text);
	void Error(std::string text);
	std::string Get();
}