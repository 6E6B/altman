#pragma once
#include <string>

namespace Backup {
    bool Export(const std::string &password);
    bool Import(const std::string &file, const std::string &password, std::string *error = nullptr);
}
