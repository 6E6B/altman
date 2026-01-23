#include "progress_overlay.h"
#include <algorithm>

#include <imgui.h>

namespace ProgressOverlay {

    namespace {
        std::vector<Task> g_tasks;
        std::mutex g_mutex;

        constexpr float CARD_WIDTH = 320.0f;
        constexpr float CARD_PADDING = 12.0f;
        constexpr float CARD_SPACING = 10.0f;
        constexpr float CARD_ROUNDING = 8.0f;
        constexpr float PROGRESS_BAR_HEIGHT = 6.0f;
        constexpr float ANIMATION_SPEED = 6.0f;
        constexpr float COMPLETION_DISPLAY_TIME = 3.0f;
        constexpr float MARGIN_RIGHT = 20.0f;
        constexpr float MARGIN_BOTTOM = 40.0f; // Above status bar
    } // namespace

    bool HasTask(const std::string &id) {
        std::lock_guard<std::mutex> lock(g_mutex);

        for (const auto &task: g_tasks) {
            if (task.id == id && !task.removing) {
                return true;
            }
        }
        return false;
    }

    void Add(const std::string &id, const std::string &title, bool cancellable, std::function<void()> onCancel) {
        std::lock_guard<std::mutex> lock(g_mutex);

        for (auto &task: g_tasks) {
            if (task.id == id) {
                task.title = title;
                task.removing = false;
                task.completed = false;
                return;
            }
        }

        Task task;
        task.id = id;
        task.title = title;
        task.cancellable = cancellable;
        task.onCancel = std::move(onCancel);
        task.animationProgress = 0.0f;
        g_tasks.push_back(std::move(task));
    }

    void Update(const std::string &id, float progress, const std::string &detail) {
        std::lock_guard<std::mutex> lock(g_mutex);

        for (auto &task: g_tasks) {
            if (task.id == id) {
                task.progress = std::clamp(progress, 0.0f, 1.0f);
                task.detail = detail;
                return;
            }
        }
    }

    void Complete(const std::string &id, bool success, const std::string &message) {
        std::lock_guard<std::mutex> lock(g_mutex);

        for (auto &task: g_tasks) {
            if (task.id == id) {
                task.completed = true;
                task.success = success;
                task.progress = 1.0f;
                task.completedTimer = 0.0f;
                if (!message.empty()) {
                    task.detail = message;
                }
                return;
            }
        }
    }

    void Remove(const std::string &id) {
        std::lock_guard<std::mutex> lock(g_mutex);

        for (auto &task: g_tasks) {
            if (task.id == id) {
                task.removing = true;
                return;
            }
        }
    }

    void Render(float deltaTime) {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (g_tasks.empty()) {
            return;
        }

        const ImGuiIO &io = ImGui::GetIO();
        const ImVec2 displaySize = io.DisplaySize;

        float yOffset = MARGIN_BOTTOM;

        for (auto it = g_tasks.begin(); it != g_tasks.end();) {
            Task &task = *it;

            if (task.removing) {
                task.animationProgress -= deltaTime * ANIMATION_SPEED;
                if (task.animationProgress <= 0.0f) {
                    it = g_tasks.erase(it);
                    continue;
                }
            } else {
                task.animationProgress = std::min(1.0f, task.animationProgress + deltaTime * ANIMATION_SPEED);
            }

            if (task.completed) {
                task.completedTimer += deltaTime;
                if (task.completedTimer >= COMPLETION_DISPLAY_TIME) {
                    task.removing = true;
                }
            }

            const float slideOffset = (1.0f - task.animationProgress) * (CARD_WIDTH + MARGIN_RIGHT);
            const float cardX = displaySize.x - CARD_WIDTH - MARGIN_RIGHT + slideOffset;
            const float cardY = displaySize.y - yOffset;

            const float titleHeight = ImGui::GetTextLineHeight();
            const float detailHeight = task.detail.empty() ? 0.0f : ImGui::GetTextLineHeight() + 4.0f;
            const float buttonHeight = task.cancellable && !task.completed ? ImGui::GetFrameHeight() + 8.0f : 0.0f;
            const float cardHeight
                = CARD_PADDING * 2 + titleHeight + PROGRESS_BAR_HEIGHT + 8.0f + detailHeight + buttonHeight;

            const ImVec2 cardPos(cardX, cardY - cardHeight);
            const ImVec2 cardSize(CARD_WIDTH, cardHeight);

            ImGui::SetNextWindowPos(cardPos);
            ImGui::SetNextWindowSize(cardSize);

            ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                                     | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;

            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, CARD_ROUNDING);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(CARD_PADDING, CARD_PADDING));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, task.animationProgress);

            ImVec4 bgColor;
            if (task.completed) {
                bgColor = task.success ? ImVec4(0.1f, 0.3f, 0.1f, 0.95f) : ImVec4(0.3f, 0.1f, 0.1f, 0.95f);
            } else {
                bgColor = ImVec4(0.15f, 0.15f, 0.18f, 0.95f);
            }
            ImGui::PushStyleColor(ImGuiCol_WindowBg, bgColor);

            const std::string windowId = "##ProgressOverlay_" + task.id;
            if (ImGui::Begin(windowId.c_str(), nullptr, flags)) {
                ImGui::Text("%s", task.title.c_str());

                if (!task.completed || task.success) {
                    ImGui::SameLine(CARD_WIDTH - CARD_PADDING * 2 - 20.0f);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));

                    if (ImGui::Button("x", ImVec2(20, 20))) {
                        task.removing = true;
                    }

                    ImGui::PopStyleColor(4);
                }

                ImGui::Spacing();

                ImVec4 progressColor;
                if (task.completed) {
                    progressColor = task.success ? ImVec4(0.3f, 0.8f, 0.3f, 1.0f) : ImVec4(0.8f, 0.3f, 0.3f, 1.0f);
                } else {
                    progressColor = ImVec4(0.4f, 0.6f, 0.9f, 1.0f);
                }

                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, progressColor);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, PROGRESS_BAR_HEIGHT / 2.0f);
                ImGui::ProgressBar(task.progress, ImVec2(-1, PROGRESS_BAR_HEIGHT), "");
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();

                if (!task.detail.empty()) {
                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
                    ImGui::TextWrapped("%s", task.detail.c_str());
                    ImGui::PopStyleColor();
                }

                if (task.cancellable && !task.completed) {
                    ImGui::Spacing();
                    if (ImGui::Button("Cancel", ImVec2(-1, 0))) {
                        if (task.onCancel) {
                            task.onCancel();
                        }
                        task.removing = true;
                    }
                }
            }
            ImGui::End();

            ImGui::PopStyleColor();
            ImGui::PopStyleVar(3);

            yOffset += cardHeight + CARD_SPACING;
            ++it;
        }
    }

    bool HasActiveTasks() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return !g_tasks.empty();
    }

    std::vector<TaskInfo> GetActiveTasks() {
        std::lock_guard<std::mutex> lock(g_mutex);

        std::vector<TaskInfo> result;
        result.reserve(g_tasks.size());

        for (const auto &task: g_tasks) {
            if (!task.removing) {
                TaskInfo info;
                info.id = task.id;
                info.title = task.title;
                info.detail = task.detail;
                info.progress = task.progress;
                info.completed = task.completed;
                info.success = task.success;
                result.push_back(std::move(info));
            }
        }

        return result;
    }

} // namespace ProgressOverlay
