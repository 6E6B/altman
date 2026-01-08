#pragma once

#include <imgui.h>
#include <deque>
#include <functional>
#include <string>
#include <format>

namespace ConfirmPopup {
    enum class PopupType {
        YesNo,
        Ok,
        Info
    };

    struct Item {
        std::string id;
        std::string message;
        std::function<void()> onYes;
        std::function<void()> onNo;
        PopupType type;
        int progress;
        bool isOpen;
        bool shouldOpen;
        bool closeable;
    };

    inline std::deque<Item> queue;
    inline int nextId = 0;

    inline void AddYesNo(const std::string& msg, std::function<void()> onYes, std::function<void()> onNo = nullptr) {
        queue.push_back({
            std::format("##ConfirmPopup{}", nextId++),
            msg,
            std::move(onYes),
            std::move(onNo),
            PopupType::YesNo,
            0,
            true,
            true,
            true
        });
    }

    inline void AddOk(const std::string& msg, std::function<void()> onOk) {
        queue.push_back({
            std::format("##ConfirmPopup{}", nextId++),
            msg,
            std::move(onOk),
            nullptr,
            PopupType::Ok,
            0,
            true,
            true,
            true
        });
    }

    inline void AddInfo(const std::string& msg) {
        queue.push_back({
            std::format("##ConfirmPopup{}", nextId++),
            msg,
            nullptr,
            nullptr,
            PopupType::Info,
            0,
            true,
            true,
            true
        });
    }

    inline void AddProgress(const std::string& msg, int percentage = 0) {
        if (!queue.empty() && queue.back().progress >= 0 && queue.back().type == PopupType::Info && !queue.back().closeable) {
            queue.back().message = msg;
            queue.back().progress = percentage;
            return;
        }

        queue.push_back({
            std::format("##ConfirmPopup{}", nextId++),
            msg,
            nullptr,
            nullptr,
            PopupType::Info,
            percentage,
            true,
            true,
            false
        });
    }

    inline void CloseProgress() {
        if (!queue.empty() && queue.back().progress >= 0 && !queue.back().closeable) {
            queue.pop_front();
        }
    }

    inline void Clear() {
        queue.clear();
    }

    inline void Render() {
        if (queue.empty()) {
            return;
        }

        Item& cur = queue.front();

        if (cur.shouldOpen) {
            ImGui::OpenPopup(cur.id.c_str());
            cur.shouldOpen = false;
        }

        ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;

        if (!cur.closeable) {
            flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar;
        }

        bool* p_open = cur.closeable ? &cur.isOpen : nullptr;

        if (ImGui::BeginPopupModal(cur.id.c_str(), p_open, flags)) {
            if (cur.closeable && !cur.isOpen) {
                ImGui::CloseCurrentPopup();
                queue.pop_front();
                ImGui::EndPopup();
                return;
            }

            ImGui::TextWrapped("%s", cur.message.c_str());
            ImGui::Spacing();

            if (cur.progress > 0) {
                float progress = static_cast<float>(cur.progress) / 100.0f;
                ImGui::ProgressBar(progress, ImVec2(300, 0), std::format("{}%", cur.progress).c_str());
            }

            switch (cur.type) {
                case PopupType::YesNo: {
                    if (ImGui::Button("Yes", ImVec2(120, 0))) {
                        if (cur.onYes) {
                            cur.onYes();
                        }
                        ImGui::CloseCurrentPopup();
                        queue.pop_front();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("No", ImVec2(120, 0))) {
                        if (cur.onNo) {
                            cur.onNo();
                        }
                        ImGui::CloseCurrentPopup();
                        queue.pop_front();
                    }
                    break;
                }

                case PopupType::Ok: {
                    if (ImGui::Button("OK", ImVec2(120, 0))) {
                        if (cur.onYes) {
                            cur.onYes();
                        }
                        ImGui::CloseCurrentPopup();
                        queue.pop_front();
                    }
                    break;
                }

                case PopupType::Info: {
                        if (ImGui::Button("OK", ImVec2(120, 0))) {
	                        ImGui::CloseCurrentPopup();
                        	queue.pop_front();
                        }
                    break;
                }
            }

            ImGui::EndPopup();
        }
    }
}