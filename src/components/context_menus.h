#pragma once

#include <imgui.h>
#include <string>
#include <functional>
#include <cstdio>

// Cross-platform safe string functions
#ifdef _WIN32
    #define SAFE_SNPRINTF(buf, size, fmt, ...) _snprintf_s(buf, size, _TRUNCATE, fmt, __VA_ARGS__)
#else
    #define SAFE_SNPRINTF(buf, size, fmt, ...) snprintf(buf, size, fmt, __VA_ARGS__)
#endif

struct StandardJoinMenuParams {
    uint64_t placeId = 0;
    uint64_t universeId = 0;
    std::string jobId;
    bool enableLaunchGame = true;
    bool enableLaunchInstance = true; // only applies if jobId not empty
    std::function<void()> onLaunchGame;      // optional
    std::function<void()> onLaunchInstance;  // optional
    std::function<void()> onFillGame;        // optional
    std::function<void()> onFillInstance;    // optional
};

inline void RenderStandardJoinMenu(const StandardJoinMenuParams &p) {
    if (ImGui::BeginMenu("Game", p.placeId != 0)) {
        if (ImGui::MenuItem("Copy Place ID", nullptr, false, p.placeId != 0)) {
            char buf[64];
            SAFE_SNPRINTF(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(p.placeId));
            ImGui::SetClipboardText(buf);
        }
        if (p.universeId != 0 && ImGui::MenuItem("Copy Universe ID")) {
            char buf[64];
            SAFE_SNPRINTF(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(p.universeId));
            ImGui::SetClipboardText(buf);
        }
        if (ImGui::BeginMenu("Copy Launch Method")) {
            char buf[512];
            if (ImGui::MenuItem("Browser Link")) {
                SAFE_SNPRINTF(buf, sizeof(buf), "https://www.roblox.com/games/start?placeId=%llu", (unsigned long long)p.placeId);
                ImGui::SetClipboardText(buf);
            }
            if (ImGui::MenuItem("Deep Link")) {
                SAFE_SNPRINTF(buf, sizeof(buf), "roblox://placeId=%llu", (unsigned long long)p.placeId);
                ImGui::SetClipboardText(buf);
            }
            if (ImGui::MenuItem("JavaScript")) {
                SAFE_SNPRINTF(buf, sizeof(buf), "Roblox.GameLauncher.joinGameInstance(%llu)", (unsigned long long)p.placeId);
                ImGui::SetClipboardText(buf);
            }
            if (ImGui::MenuItem("Roblox Luau")) {
                SAFE_SNPRINTF(buf, sizeof(buf), "game:GetService(\"TeleportService\"):Teleport(%llu)", (unsigned long long)p.placeId);
                ImGui::SetClipboardText(buf);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Fill Join Options", nullptr, false, p.placeId != 0)) {
            if (p.onFillGame) p.onFillGame();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Launch", nullptr, false, p.enableLaunchGame && p.placeId != 0)) {
            if (p.onLaunchGame) p.onLaunchGame();
        }
        ImGui::EndMenu();
    }

    // Instance submenu (only when jobId exists)
    if (!p.jobId.empty() && ImGui::BeginMenu("Instance")) {
        if (ImGui::MenuItem("Copy Job ID")) {
            ImGui::SetClipboardText(p.jobId.c_str());
        }
        if (ImGui::BeginMenu("Copy Launch Method")) {
            if (ImGui::MenuItem("Browser Link")) {
                std::string link = "https://www.roblox.com/games/start?placeId=" + std::to_string(p.placeId) + "&gameInstanceId=" + p.jobId;
                ImGui::SetClipboardText(link.c_str());
            }
            if (ImGui::MenuItem("Deep Link")) {
                char buf[512];
                SAFE_SNPRINTF(buf, sizeof(buf), "roblox://placeId=%llu&gameInstanceId=%s", (unsigned long long)p.placeId, p.jobId.c_str());
                ImGui::SetClipboardText(buf);
            }
            if (ImGui::MenuItem("JavaScript")) {
                std::string js = "Roblox.GameLauncher.joinGameInstance(" + std::to_string(p.placeId) + ", \"" + p.jobId + "\")";
                ImGui::SetClipboardText(js.c_str());
            }
            if (ImGui::MenuItem("Roblox Luau")) {
                std::string luau = "game:GetService(\"TeleportService\"):TeleportToPlaceInstance(" + std::to_string(p.placeId) + ", \"" + p.jobId + "\")";
                ImGui::SetClipboardText(luau.c_str());
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Fill Join Options", nullptr, false, p.placeId != 0)) {
            if (p.onFillInstance) p.onFillInstance();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Launch", nullptr, false, p.enableLaunchInstance && p.placeId != 0)) {
            if (p.onLaunchInstance) p.onLaunchInstance();
        }
        ImGui::EndMenu();
    }
}