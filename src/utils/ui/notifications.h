#pragma once

#include <string>

#ifdef _WIN32
    #include <windows.h>
    #include <shellapi.h>
#endif

namespace Notifications {
#ifdef _WIN32
    // Global variable to store the main application HWND
    extern HWND g_appHWnd;

    // Initialization function to set the HWND
    inline void Initialize(HWND mainWindowHandle) {
        g_appHWnd = mainWindowHandle;
    }

    inline bool showNotification(
        const std::wstring &title,
        const std::wstring &text) {
        if (!g_appHWnd) {
            // HWND not initialized, cannot show notification
            return false;
        }

        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(NOTIFYICONDATAW);
        nid.hWnd = g_appHWnd;
        nid.uID = 1;

        Shell_NotifyIconW(NIM_DELETE, &nid);

        nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        if (!nid.hIcon) {
            return false;
        }

        nid.uFlags = NIF_ICON | NIF_TIP | NIF_INFO | NIF_STATE;

        nid.dwState = NIS_HIDDEN;
        nid.dwStateMask = NIS_HIDDEN;

        wcsncpy_s(nid.szTip, L"AltMan Notification", _TRUNCATE);

        nid.dwInfoFlags = NIIF_NONE;

        wcsncpy_s(nid.szInfoTitle, title.substr(0, 63).c_str(), _TRUNCATE);
        wcsncpy_s(nid.szInfo, text.substr(0, 255).c_str(), _TRUNCATE);

        BOOL success = Shell_NotifyIconW(NIM_ADD, &nid);

        if (success) {
            NOTIFYICONDATAW versionNid = {};
            versionNid.cbSize = sizeof(NOTIFYICONDATAW);
            versionNid.hWnd = g_appHWnd;
            versionNid.uID = nid.uID;
            versionNid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &versionNid);
        }
        return success;
    }

#elif __APPLE__
    // TODO: Get rid of this notification system and use imgui notifications
    // macOS implementation using NSUserNotificationCenter or UserNotifications framework
    
    // No initialization needed on macOS
    inline void Initialize(void* /* unused */) {
        // No-op on macOS
    }

    // Convert std::wstring to std::string for macOS
    inline std::string wstringToString(const std::wstring &wstr) {
        if (wstr.empty()) return std::string();
        std::string result;
        result.reserve(wstr.size());
        for (wchar_t wc : wstr) {
            if (wc < 128) result.push_back(static_cast<char>(wc));
            else result.push_back('?'); // Simple fallback for non-ASCII
        }
        return result;
    }

    inline bool showNotification(
        const std::wstring &title,
        const std::wstring &text) {
        // Convert wstring to string for macOS
        std::string titleStr = wstringToString(title);
        std::string textStr = wstringToString(text);
        
        // Use AppleScript to show notification (simple cross-process solution)
        // For a more robust solution, you'd want to use NSUserNotificationCenter
        std::string command = "osascript -e 'display notification \"" + 
                             textStr + "\" with title \"" + titleStr + "\"'";
        
        int result = system(command.c_str());
        return result == 0;
    }

    // Overload for std::string (more natural on macOS)
    inline bool showNotification(
        const std::string &title,
        const std::string &text) {
        std::string command = "osascript -e 'display notification \"" + 
                             text + "\" with title \"" + title + "\"'";
        
        int result = system(command.c_str());
        return result == 0;
    }

#else
    // Stub for other platforms
    inline void Initialize(void* /* unused */) {}
    
    inline bool showNotification(
        const std::wstring & /* title */,
        const std::wstring & /* text */) {
        return false;
    }
#endif
}