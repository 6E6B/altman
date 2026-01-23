#pragma once

#include <functional>
#include <string>

enum class NotificationPosition : uint8_t {
    TopRight,
    TopLeft,
    BottomRight,
    BottomLeft
};

class UpdateNotification {
    public:
        struct Notification {
                std::string title;
                std::string message;
                NotificationPosition position {NotificationPosition::TopRight};
                float lifetime;
                float elapsed {0.0f};
                bool canDismiss {true};
                bool markedForRemoval {false};
                std::function<void()> onClick;
                uint64_t id;

                Notification(std::string title, std::string message, float life = 5.0f);
        };

        static void
        Show(std::string title, std::string message, float lifetime = 5.0f, std::function<void()> onClick = nullptr);

        static void Show(
            std::string title,
            std::string message,
            NotificationPosition position,
            float lifetime = 5.0f,
            std::function<void()> onClick = nullptr
        );

        static uint64_t ShowPersistent(std::string title, std::string message, std::function<void()> onClick = nullptr);

        static uint64_t ShowPersistent(
            std::string title,
            std::string message,
            NotificationPosition position,
            std::function<void()> onClick = nullptr
        );

        static void Dismiss(uint64_t id) noexcept;
        static void Update(float deltaTime) noexcept;
        static void Render();
        static void Clear() noexcept;
        static size_t Count() noexcept;

        static constexpr size_t MaxNotifications = 8;
};
