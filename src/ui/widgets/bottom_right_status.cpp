#include "bottom_right_status.h"
#include "modal_popup.h"

#include <mutex>
#include <thread>
#include <chrono>
#include <format>

namespace ButtonRightStatus {
    namespace {
        constexpr int COUNTDOWN_SECONDS = 5;
        constexpr const char* IDLE_TEXT = "Idle";

        std::mutex g_mutex;
        std::string g_originalText = IDLE_TEXT;
        std::string g_displayText = IDLE_TEXT;
        std::chrono::steady_clock::time_point g_lastSetTime{};

        void UpdateDisplayText(std::string_view text, int secondsRemaining) {
            if (secondsRemaining > 0) {
                g_displayText = std::format("{} ({})", text, secondsRemaining);
            } else {
                g_displayText = IDLE_TEXT;
                g_originalText = IDLE_TEXT;
            }
        }

        bool IsCurrentTimer(const std::chrono::steady_clock::time_point& timerStart) {
            return g_lastSetTime == timerStart;
        }
    }

    void Set(std::string text) {
        const auto timerStart = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_originalText = text;
            g_displayText = std::format("{} ({})", text, COUNTDOWN_SECONDS);
            g_lastSetTime = timerStart;
        }

        std::thread([timerStart, text = std::move(text)]() {
            for (int i = COUNTDOWN_SECONDS; i >= 0; --i) {
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    if (!IsCurrentTimer(timerStart)) {
                        return;
                    }
                }

                std::this_thread::sleep_for(std::chrono::seconds(1));

                std::lock_guard<std::mutex> lock(g_mutex);
                if (IsCurrentTimer(timerStart)) {
                    UpdateDisplayText(text, i - 1);
                } else {
                    return;
                }
            }
        }).detach();
    }

    void Error(std::string text) {
        Set(text);
        ModalPopup::AddInfo(text);
    }

    std::string Get() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_displayText;
    }
}