#pragma once
#include "modal_popup.h"
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

using namespace std;

namespace Status {
	inline mutex _mtx;
	inline string _originalText = "Idle";
	inline string _displayText = "Idle";

	inline chrono::steady_clock::time_point _lastSetTime {};

	inline void Set(const string &s) {
		auto tp = chrono::steady_clock::now();
		{
			lock_guard<mutex> lock(_mtx);
			_originalText = s;
			_displayText = _originalText + " (5)";
			_lastSetTime = tp;
		}

		thread([tp, s]() {
			for (int i = 5; i >= 0; --i) {
				{
					lock_guard<mutex> lock(_mtx);
					if (_lastSetTime != tp) { return; }
				}

				this_thread::sleep_for(chrono::seconds(1));
				lock_guard<mutex> lock(_mtx);

				if (_lastSetTime == tp) {
					if (i > 0) {
						_displayText = s + " (" + to_string(i - 1) + ")";
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

	inline void Error(const string &s) {
		Set(s);
		ModalPopup::Add(s);
	}

	inline string Get() {
		lock_guard<mutex> lock(_mtx);
		return _displayText;
	}
} // namespace Status
