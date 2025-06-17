#pragma once
#include <string>
#include <windows.h>
#include <nlohmann/json.hpp>
#include "../network/http.hpp"
#include "../core/logging.hpp"
#include "main_thread.h"
#include "../ui/confirm.h"
#include "threading.h"
#include "../../version.h"

inline void CheckForUpdates() {
    Threading::newThread([]() {
        const std::string url = "https://api.github.com/repos/crowsyndrome/altman/releases/latest";
        auto resp = HttpClient::get(url, {{"User-Agent","AltMan"}, {"Accept","application/vnd.github+json"}});
        if (resp.status_code != 200) {
            LOG_ERROR("Failed to check for updates: HTTP " + std::to_string(resp.status_code));
            return;
        }
        nlohmann::json j = HttpClient::decode(resp);
        std::string latest = j.value("tag_name", "");
        std::string html = j.value("html_url", "");
        if (!latest.empty() && (latest[0]=='v' || latest[0]=='V'))
            latest = latest.substr(1);
        if (!latest.empty() && latest != APP_VERSION) {
            MainThread::Post([latest, html]() {
                std::string msg = "A new version (" + latest + ") is available. Download?";
                ConfirmPopup::Add(msg, [html]() {
                    ShellExecuteA(NULL, "open", html.c_str(), NULL, NULL, SW_SHOWNORMAL);
                });
            });
        }
    });
}
