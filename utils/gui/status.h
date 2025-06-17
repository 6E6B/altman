#pragma once
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include "modal_popup.h"

using namespace std;

namespace Status {
    inline mutex _mtx;
    inline string _originalText = "Idle";
    inline string _displayText = "Idle";

    inline chrono::steady_clock::time_point _lastSetTime{};

    inline void Set(const string &s) {
        auto tp = chrono::steady_clock::now(); {
            lock_guard<mutex> lock(_mtx);
            _originalText = s;
            _displayText = _originalText + " (5)"; // Initial countdown display
            _lastSetTime = tp;
        }

        thread([tp, s]() {
            for (int i = 5; i >= 0; --i) // Loop from 5 down to 0
            {
                {
                    lock_guard<mutex> lock(_mtx);
                    if (_lastSetTime != tp) {
                        // Another Status::Set call was made, this thread should exit
                        return;
                    }
                }

                this_thread::sleep_for(chrono::seconds(1));
                lock_guard<mutex> lock(_mtx);

                if (_lastSetTime == tp) // Check if this thread is still responsible for the current status
                {
                    if (i > 0) // For countdown values 4 down to 0
                    {
                        _displayText = s + " (" + to_string(i - 1) + ")";
                    } else // When countdown reaches 0 (after displaying " (0)")
                    {
                        _displayText = "Idle"; // Reset to Idle
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
}
