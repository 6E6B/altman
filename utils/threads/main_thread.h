#pragma once
#include <functional>
#include <deque>
#include <mutex>

namespace MainThread {
    using Task = std::function<void()>;
    inline std::deque<Task> tasks;
    inline std::mutex mtx;

    inline void Post(Task t) {
        std::lock_guard<std::mutex> lock(mtx);
        tasks.push_back(std::move(t));
    }

    inline void Process() {
        std::deque<Task> toRun;
        {
            std::lock_guard<std::mutex> lock(mtx);
            toRun.swap(tasks);
        }
        for (auto &t : toRun) {
            t();
        }
    }
}
