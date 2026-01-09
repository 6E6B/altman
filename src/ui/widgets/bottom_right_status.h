#pragma once

#include <string>

namespace Status {
	void Set(const std::string& s);
	void Error(const std::string& s);
	std::string Get();
}