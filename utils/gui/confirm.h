#pragma once

#include <imgui.h>
#include <deque>
#include <functional>
#include <string>

namespace ConfirmPopup {
    struct Item {
        std::string message;
        std::function<void()> onYes;
        bool open = true;
    };

    inline std::deque<Item> queue;

    inline void Add(const std::string &msg, std::function<void()> onYes) {
        queue.push_back({msg, std::move(onYes), true});
    }

    inline void Render() {
        if (queue.empty())
            return;

        Item &cur = queue.front();
        if (cur.open) {
            ImGui::OpenPopup("Confirm");
            cur.open = false;
        }

        if (ImGui::BeginPopupModal("Confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", cur.message.c_str());
            ImGui::Spacing();
            if (ImGui::Button("Yes", ImVec2(120, 0))) {
                if (cur.onYes)
                    cur.onYes();
                ImGui::CloseCurrentPopup();
                queue.pop_front();
            }
            ImGui::SameLine();
            if (ImGui::Button("No", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
                queue.pop_front();
            }
            ImGui::EndPopup();
        }
    }
}
