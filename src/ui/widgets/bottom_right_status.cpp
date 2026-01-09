#include "bottom_right_status.h"
#include "modal_popup.h"

#include <mutex>
#include <thread>
#include <chrono>

namespace Status {
	namespace {
		std::mutex mtx;
		std::string originalText = "Idle";
		std::string displayText = "Idle";
		std::chrono::steady_clock::time_point lastSetTime{};
	}

	void Set(const std::string& s) {
		auto tp = std::chrono::steady_clock::now();

		{
			std::lock_guard<std::mutex> lock(mtx);
			originalText = s;
			displayText = originalText + " (5)";
			lastSetTime = tp;
		}

		std::thread([tp, s]() {
			for (int i = 5; i >= 0; --i) {
				{
					std::lock_guard<std::mutex> lock(mtx);
					if (lastSetTime != tp) {
						return;
					}
				}

				std::this_thread::sleep_for(std::chrono::seconds(1));
				std::lock_guard<std::mutex> lock(mtx);

				if (lastSetTime == tp) {
					if (i > 0) {
						displayText = s + " (" + std::to_string(i - 1) + ")";
					} else {
						displayText = "Idle";
						originalText = "Idle";
					}
				} else {
					return;
				}
			}
		}).detach();
	}

	void Error(const std::string& s) {
		Set(s);
		ModalPopup::AddInfo(s);
	}

	std::string Get() {
		std::lock_guard<std::mutex> lock(mtx);
		return displayText;
	}
}