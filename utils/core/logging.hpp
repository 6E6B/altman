#pragma once
#include "../../components/console/console.h"
#include "../ui/modal_popup.h"
#include <string>

#define LOG(msg) Console::Log(msg)
#define LOG_INFO(msg) Console::Log(std::string("[INFO] ") + (msg))
#define LOG_WARN(msg) do { \
    Console::Log(std::string("[WARN] ") + (msg)); \
    ModalPopup::Add(std::string("Warning: ") + (msg)); \
} while(0)
#define LOG_ERROR(msg) do { \
    Console::Log(std::string("[ERROR] ") + (msg)); \
    ModalPopup::Add(std::string("Error: ") + (msg)); \
} while(0)
