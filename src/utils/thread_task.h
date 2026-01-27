#pragma once
#include <thread>
#include <utility>
#include <functional>
#include <deque>
#include <mutex>

namespace ThreadTask {

	using Task = std::function<void()>;
	inline std::deque<Task> tasks;
	inline std::mutex mtx;

	inline void RunOnMain(Task t) {
		std::lock_guard<std::mutex> lock(mtx);
		tasks.push_back(std::move(t));
	}

	inline void RunOnMainUpdate() {
		std::deque<Task> toRun;
		{
			std::lock_guard<std::mutex> lock(mtx);
			toRun.swap(tasks);
		}
		for (auto &t: toRun) {
			t();
		}
	}


	// Launches f(args...) on a detached background thread.
	template<typename Func, typename... Args>
	void fireAndForget(Func &&f, Args &&... args) {
		std::thread(
			[fn = std::forward<Func>(f),
				tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
				std::apply(fn, tup);
			}
		).detach();
	}
}
