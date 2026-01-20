#include "context_menus.h"

#include <imgui.h>
#include <format>
#include <cstdio>

void RenderStandardJoinMenu(const StandardJoinMenuParams& p) {
    if (ImGui::BeginMenu("Game", p.placeId != 0)) {
        if (ImGui::MenuItem("Copy Place ID", nullptr, false, p.placeId != 0)) {
        	auto text = std::format("{}", p.placeId);
            ImGui::SetClipboardText(text.c_str());
        }
        
        if (p.universeId != 0 && ImGui::MenuItem("Copy Universe ID")) {
        	auto text = std::format("{}", p.universeId);
            ImGui::SetClipboardText(text.c_str());
        }
        
        if (ImGui::BeginMenu("Copy Launch Method")) {
            if (ImGui::MenuItem("Browser Link")) {
            	auto text = std::format("https://www.roblox.com/games/start?placeId={}", p.placeId);
                ImGui::SetClipboardText(text.c_str());
            }
            
            if (ImGui::MenuItem("Deep Link")) {
            	auto text = std::format("roblox://placeId={}", p.placeId);
                ImGui::SetClipboardText(text.c_str());
            }
            
            if (ImGui::MenuItem("JavaScript")) {
            	auto text = std::format("Roblox.GameLauncher.joinGameInstance({})", p.placeId);
                ImGui::SetClipboardText(text.c_str());
            }
            
            if (ImGui::MenuItem("Roblox Luau")) {
            	auto text = std::format("game:GetService(\"TeleportService\"):Teleport({})", p.placeId);
                ImGui::SetClipboardText(text.c_str());
            }
            
            ImGui::EndMenu();
        }
        
        ImGui::Separator();
        
        if (ImGui::MenuItem("Fill Join Options", nullptr, false, p.placeId != 0)) {
            if (p.onFillGame) {
                p.onFillGame();
            }
        }
        
        ImGui::Separator();
        
        if (ImGui::MenuItem("Launch", nullptr, false, p.enableLaunchGame && p.placeId != 0)) {
            if (p.onLaunchGame) {
                p.onLaunchGame();
            }
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
                std::string link = std::format("https://www.roblox.com/games/start?placeId={}&gameInstanceId={}", 
                    p.placeId, p.jobId);
                ImGui::SetClipboardText(link.c_str());
            }
            
            if (ImGui::MenuItem("Deep Link")) {
            	auto text = std::format("roblox://placeId={}&gameInstanceId={}", p.placeId, p.jobId);
                ImGui::SetClipboardText(text.c_str());
            }
            
            if (ImGui::MenuItem("JavaScript")) {
                std::string js = std::format("Roblox.GameLauncher.joinGameInstance({}, \"{}\")", 
                    p.placeId, p.jobId);
                ImGui::SetClipboardText(js.c_str());
            }
            
            if (ImGui::MenuItem("Roblox Luau")) {
                std::string luau = std::format("game:GetService(\"TeleportService\"):TeleportToPlaceInstance({}, \"{}\")", 
                    p.placeId, p.jobId);
                ImGui::SetClipboardText(luau.c_str());
            }
            
            ImGui::EndMenu();
        }
        
        ImGui::Separator();
        
        if (ImGui::MenuItem("Fill Join Options", nullptr, false, p.placeId != 0)) {
            if (p.onFillInstance) {
                p.onFillInstance();
            }
        }
        
        ImGui::Separator();
        
        if (ImGui::MenuItem("Launch", nullptr, false, p.enableLaunchInstance && p.placeId != 0)) {
            if (p.onLaunchInstance) {
                p.onLaunchInstance();
            }
        }
        
        ImGui::EndMenu();
    }
}