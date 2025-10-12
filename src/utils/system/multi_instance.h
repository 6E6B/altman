#pragma once
#ifdef _WIN32
#	include <windows.h>

namespace MultiInstance {
	inline HANDLE g_mutex = nullptr;

	inline void Enable() {
		if (!g_mutex) { g_mutex = CreateMutexW(nullptr, FALSE, L"ROBLOX_singletonEvent"); }
	}

	inline void Disable() {
		if (g_mutex) {
			CloseHandle(g_mutex);
			g_mutex = nullptr;
		}
	}
} // namespace MultiInstance
#endif
