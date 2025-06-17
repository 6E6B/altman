#pragma once

#include <windows.h>
#include <string>
#include <shellapi.h>

namespace Notifications {
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
            // Optionally, log an error or handle this case
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
}
