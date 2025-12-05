#pragma once
#ifdef _WIN32
#include <windows.h>

namespace MultiInstance {
inline HANDLE g_mutex = nullptr;

inline void Enable() {
    if (!g_mutex)
        g_mutex = CreateMutexW(nullptr, FALSE, L"ROBLOX_singletonEvent");
}

inline void Disable() {
    if (g_mutex) {
        CloseHandle(g_mutex);
        g_mutex = nullptr;
    }
}
}
#elif __APPLE__
#include <semaphore.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace MultiInstance {
inline sem_t* g_semaphore = SEM_FAILED;
inline const char* SEMAPHORE_NAME = "/roblox_singleton_event";

inline void Enable() {
    if (g_semaphore == SEM_FAILED) {
        // Try to create/open the semaphore
        // O_CREAT creates it if it doesn't exist
        g_semaphore = sem_open(SEMAPHORE_NAME, O_CREAT, 0644, 1);
        if (g_semaphore == SEM_FAILED) {
            // Failed to create semaphore
            return;
        }
    }
}

inline void Disable() {
    if (g_semaphore != SEM_FAILED) {
        sem_close(g_semaphore);
        sem_unlink(SEMAPHORE_NAME); // Remove the semaphore
        g_semaphore = SEM_FAILED;
    }
}
}
#endif