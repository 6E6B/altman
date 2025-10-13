#pragma once
#include "modal_popup.h"
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace Status {
	inline std::mutex _mtx;
	inline std::string _originalText = "Idle";
	inline std::string _displayText = "Idle";

	inline std::chrono::steady_clock::time_point _lastSetTime {};

	inline void Set(const std::string &s) {
		auto tp = std::chrono::steady_clock::now();
		{
			std::lock_guard<std::mutex> lock(_mtx);
			_originalText = s;
			_displayText = _originalText + " (5)";
			_lastSetTime = tp;
		}

		std::thread([tp, s]() {
			for (int i = 5; i >= 0; --i) {
				{
					std::lock_guard<std::mutex> lock(_mtx);
					if (_lastSetTime != tp) { return; }
				}

				std::this_thread::sleep_for(std::chrono::seconds(1));
				std::lock_guard<std::mutex> lock(_mtx);

				if (_lastSetTime == tp) {
					if (i > 0) {
						_displayText = s + " (" + std::to_string(i - 1) + ")";
					} else {
						_displayText = "Idle";
						_originalText = "Idle";
					}
				} else {
					return;
				}
			}
		}).detach();
	}

	inline void Error(const std::string &s) {
		Set(s);
		ModalPopup::Add(s);
	}

	inline std::string Get() {
		std::lock_guard<std::mutex> lock(_mtx);
		return _displayText;
	}
} // namespace Status
