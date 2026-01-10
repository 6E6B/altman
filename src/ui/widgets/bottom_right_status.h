#pragma once

#include <string>

namespace Status {
	void Set(std::string text);
	void Error(std::string text);
	std::string Get();
}