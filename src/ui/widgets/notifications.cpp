#include "notifications.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <vector>

#include <imgui.h>

namespace {
    std::vector<UpdateNotification::Notification> activeNotifications;
    std::mutex notificationMutex;
    uint64_t nextNotificationId = 0;

    struct PositionOffsets {
            float topLeft = 20.0f;
            float topRight = 20.0f;
            float bottomLeft = 20.0f;
            float bottomRight = 20.0f;

            void reset() noexcept {
                topLeft = topRight = bottomLeft = bottomRight = 20.0f;
            }

            float &get(NotificationPosition pos) noexcept {
                switch (pos) {
                    case NotificationPosition::TopLeft:
                        return topLeft;
                    case NotificationPosition::TopRight:
                        return topRight;
                    case NotificationPosition::BottomLeft:
                        return bottomLeft;
                    case NotificationPosition::BottomRight:
                        return bottomRight;
                }
                return topRight;
            }
    };
} // namespace

UpdateNotification::Notification::Notification(std::string t, std::string m, float life) :
    title(std::move(t)),
    message(std::move(m)),
    lifetime(life),
    id(nextNotificationId++) {
}

void UpdateNotification::Show(std::string title, std::string message, float lifetime, std::function<void()> onClick) {
    Show(std::move(title), std::move(message), NotificationPosition::TopRight, lifetime, std::move(onClick));
}

void UpdateNotification::Show(
    std::string title,
    std::string message,
    NotificationPosition position,
    float lifetime,
    std::function<void()> onClick
) {
    std::lock_guard lock(notificationMutex);

    while (activeNotifications.size() >= MaxNotifications) {
        activeNotifications.erase(activeNotifications.begin());
    }

    Notification notif(std::move(title), std::move(message), lifetime);
    notif.position = position;
    notif.onClick = std::move(onClick);
    activeNotifications.push_back(std::move(notif));
}

uint64_t UpdateNotification::ShowPersistent(std::string title, std::string message, std::function<void()> onClick) {
    return ShowPersistent(std::move(title), std::move(message), NotificationPosition::TopRight, std::move(onClick));
}

uint64_t UpdateNotification::ShowPersistent(
    std::string title,
    std::string message,
    NotificationPosition position,
    std::function<void()> onClick
) {
    std::lock_guard lock(notificationMutex);

    while (activeNotifications.size() >= MaxNotifications) {
        activeNotifications.erase(activeNotifications.begin());
    }

    Notification notif(std::move(title), std::move(message), -1.0f);
    notif.position = position;
    notif.onClick = std::move(onClick);
    notif.canDismiss = true;

    const uint64_t id = notif.id;
    activeNotifications.push_back(std::move(notif));
    return id;
}

void UpdateNotification::Dismiss(uint64_t id) noexcept {
    std::lock_guard lock(notificationMutex);
    for (auto &notif: activeNotifications) {
        if (notif.id == id) {
            notif.markedForRemoval = true;
            break;
        }
    }
}

void UpdateNotification::Update(float deltaTime) noexcept {
    std::lock_guard lock(notificationMutex);

    std::erase_if(activeNotifications, [deltaTime](Notification &notif) {
        if (notif.markedForRemoval) {
            return true;
        }
        if (notif.lifetime > 0.0f) {
            notif.elapsed += deltaTime;
            return notif.elapsed >= notif.lifetime;
        }
        return false;
    });
}

void UpdateNotification::Render() {
    std::lock_guard lock(notificationMutex);

    if (activeNotifications.empty()) {
        return;
    }

    ImGuiIO &io = ImGui::GetIO();

    constexpr float WINDOW_WIDTH = 300.0f;
    constexpr float BUTTON_SIZE = 20.0f;
    constexpr float WINDOW_PADDING = 12.0f;
    constexpr float ROUNDING = 8.0f;
    constexpr float SPACING_INCREMENT = 130.0f;
    constexpr float EDGE_MARGIN = 20.0f;

    PositionOffsets offsets;

    for (auto &notif: activeNotifications) {
        float &yOffset = offsets.get(notif.position);

        const bool isBottom
            = (notif.position == NotificationPosition::BottomLeft
               || notif.position == NotificationPosition::BottomRight);
        const bool isLeft
            = (notif.position == NotificationPosition::TopLeft || notif.position == NotificationPosition::BottomLeft);

        const float xPos = isLeft ? EDGE_MARGIN : (io.DisplaySize.x - WINDOW_WIDTH - EDGE_MARGIN);
        const float yPos = isBottom ? (io.DisplaySize.y - yOffset - 110.0f) : yOffset;

        ImGui::SetNextWindowPos(ImVec2(xPos, yPos));
        ImGui::SetNextWindowSize(ImVec2(WINDOW_WIDTH, 0));

        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                                           | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ROUNDING);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(WINDOW_PADDING, WINDOW_PADDING));

        char windowName[32];
        snprintf(windowName, sizeof(windowName), "##Notif%llu", static_cast<unsigned long long>(notif.id));

        if (ImGui::Begin(windowName, nullptr, flags)) {
            if (notif.canDismiss) {
                const float buttonX = ImGui::GetWindowWidth() - BUTTON_SIZE - WINDOW_PADDING;
                ImGui::SetCursorPosX(buttonX);

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 0.5f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.9f, 0.1f, 0.1f, 0.7f));

                if (ImGui::Button("X", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
                    notif.markedForRemoval = true;
                }
                ImGui::PopStyleColor(3);

                ImGui::SetCursorPosY(ImGui::GetStyle().WindowPadding.y);
            }

            if (io.Fonts && !io.Fonts->Fonts.empty()) {
                ImGui::PushFont(io.Fonts->Fonts[0]);
                ImGui::TextWrapped("%s", notif.title.c_str());
                ImGui::PopFont();
            } else {
                ImGui::TextWrapped("%s", notif.title.c_str());
            }

            ImGui::Spacing();
            ImGui::TextWrapped("%s", notif.message.c_str());

            if (notif.onClick) {
                ImGui::Spacing();
                if (ImGui::Button("View", ImVec2(-1, 0))) {
                    notif.onClick();
                }
            }

            if (notif.lifetime > 0.0f) {
                const float progress = std::clamp(notif.elapsed / notif.lifetime, 0.0f, 1.0f);
                ImGui::ProgressBar(progress, ImVec2(-1, 2), "");
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(2);

        yOffset += SPACING_INCREMENT;
    }
}

void UpdateNotification::Clear() noexcept {
    std::lock_guard lock(notificationMutex);
    activeNotifications.clear();
}

size_t UpdateNotification::Count() noexcept {
    std::lock_guard lock(notificationMutex);
    return activeNotifications.size();
}
