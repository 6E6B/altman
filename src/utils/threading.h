#pragma once
#include <thread>
#include <utility>

namespace Threading {
	// Launches f(args...) on a detached background thread.
	template<typename Func, typename... Args>
	void newThread(Func &&f, Args &&... args) {
		std::thread(
			[fn = std::forward<Func>(f),
				tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
				std::apply(fn, tup);
			}
		).detach();
	}
}
