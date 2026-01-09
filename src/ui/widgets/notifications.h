#pragma once

#include <string>
#include <functional>
#include <cstdint>

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
			NotificationPosition position{NotificationPosition::TopRight};
			float lifetime;
			float elapsed{0.0f};
			bool canDismiss{true};
			std::function<void()> onClick;

			Notification(std::string_view t, std::string_view m, float life = 5.0f);
		};

		static void Show(std::string_view title, std::string_view message,
						float lifetime = 5.0f, std::function<void()> onClick = nullptr);

		static void ShowPersistent(std::string_view title, std::string_view message,
								  std::function<void()> onClick = nullptr);

		static void Update(float deltaTime) noexcept;
		static void Render();
		static void Clear() noexcept;
};