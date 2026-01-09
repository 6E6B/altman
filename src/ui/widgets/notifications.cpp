#include "notifications.h"
#include <format>
#include <imgui.h>
#include <vector>

namespace {
    std::vector<UpdateNotification::Notification> activeNotifications;
}

UpdateNotification::Notification::Notification(std::string_view t, std::string_view m, float life)
    : title(t), message(m), lifetime(life) {}

void UpdateNotification::Show(std::string_view title, std::string_view message,
                float lifetime, std::function<void()> onClick) {
    Notification notif(title, message, lifetime);
    notif.onClick = std::move(onClick);
    activeNotifications.push_back(std::move(notif));
}

void UpdateNotification::ShowPersistent(std::string_view title, std::string_view message,
                          std::function<void()> onClick) {
    Notification notif(title, message, -1.0f);
    notif.onClick = std::move(onClick);
    notif.canDismiss = false;
    activeNotifications.push_back(std::move(notif));
}

void UpdateNotification::Update(float deltaTime) noexcept {
    std::erase_if(activeNotifications, [deltaTime](Notification& notif) {
        if (notif.lifetime > 0.0f) {
            notif.elapsed += deltaTime;
            return notif.elapsed >= notif.lifetime;
        }
        return false;
    });
}

void UpdateNotification::Render() {
    if (activeNotifications.empty()) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    float yOffset = 20.0f;

    constexpr float WINDOW_WIDTH = 300.0f;
    constexpr float BUTTON_SIZE = 20.0f;
    constexpr float WINDOW_PADDING = 12.0f;
    constexpr float ROUNDING = 8.0f;
    constexpr float SPACING_INCREMENT = 130.0f;

    for (auto& notif : activeNotifications) {
        const ImVec2 windowPos = [&]() {
            switch (notif.position) {
                case NotificationPosition::TopRight:
                    return ImVec2(io.DisplaySize.x - WINDOW_WIDTH - 20.0f, yOffset);
                case NotificationPosition::TopLeft:
                    return ImVec2(20.0f, yOffset);
                case NotificationPosition::BottomRight:
                    return ImVec2(io.DisplaySize.x - WINDOW_WIDTH - 20.0f, io.DisplaySize.y - 150.0f);
                case NotificationPosition::BottomLeft:
                default:
                    return ImVec2(20.0f, io.DisplaySize.y - 150.0f);
            }
        }();

        ImGui::SetNextWindowPos(windowPos);
        ImGui::SetNextWindowSize(ImVec2(WINDOW_WIDTH, 0));

        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                          ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_AlwaysAutoResize;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ROUNDING);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(WINDOW_PADDING, WINDOW_PADDING));

        const auto windowLabel = std::format("##Notification{}", static_cast<const void*>(&notif));
        if (ImGui::Begin(windowLabel.c_str(), nullptr, flags)) {
            if (notif.canDismiss) {
                ImGui::SameLine(ImGui::GetWindowWidth() - BUTTON_SIZE - 10.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 0.5f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.9f, 0.1f, 0.1f, 0.7f));

                if (ImGui::Button("X", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                    notif.elapsed = notif.lifetime;
                }
                ImGui::PopStyleColor(3);
            }

            ImGui::PushFont(io.Fonts->Fonts[0]);
            ImGui::TextWrapped("%s", notif.title.c_str());
            ImGui::PopFont();

            ImGui::Spacing();
            ImGui::TextWrapped("%s", notif.message.c_str());

            if (notif.onClick) {
                ImGui::Spacing();
                if (ImGui::Button("View", ImVec2(-1, 0))) {
                    notif.onClick();
                }
            }

            if (notif.lifetime > 0.0f) {
                const float progress = notif.elapsed / notif.lifetime;
                ImGui::ProgressBar(progress, ImVec2(-1, 2));
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(2);

        yOffset += SPACING_INCREMENT;
    }
}

void UpdateNotification::Clear() noexcept {
    activeNotifications.clear();
}