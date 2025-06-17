#pragma once
#include <imgui.h>
#include <deque>
#include <string>

namespace ModalPopup {
    struct Notification {
        std::string message;
        bool open = true;
    };

    inline std::deque<Notification> queue;

    inline void Add(const std::string &msg) {
        queue.push_back({msg, true});
    }

    inline void Render() {
        if (queue.empty()) return;
        Notification &current = queue.front();
        if (current.open) {
            ImGui::OpenPopup("Notification");
            current.open = false;
        }
        if (ImGui::BeginPopupModal("Notification", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", current.message.c_str());
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(300, 0))) {
                ImGui::CloseCurrentPopup();
                queue.pop_front();
            }
            ImGui::EndPopup();
        }
    }
}
