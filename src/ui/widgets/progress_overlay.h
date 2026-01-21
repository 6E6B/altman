#pragma once

#include <functional>
#include <string>
#include <vector>
#include <mutex>

namespace ProgressOverlay {

	struct Task {
		std::string id;
		std::string title;
		std::string detail;
		float progress = 0.0f;
		bool cancellable = false;
		bool minimized = false;
		std::function<void()> onCancel;

		float animationProgress = 0.0f;  // 0 = hidden, 1 = fully visible
		bool removing = false;

		bool completed = false;
		bool success = true;
		float completedTimer = 0.0f;
	};

	struct TaskInfo {
		std::string id;
		std::string title;
		std::string detail;
		float progress = 0.0f;
		bool completed = false;
		bool success = true;
	};

	bool HasTask(const std::string& id);

	void Add(const std::string& id, const std::string& title, bool cancellable = false,
			 std::function<void()> onCancel = nullptr);

	void Update(const std::string& id, float progress, const std::string& detail);

	void Complete(const std::string& id, bool success, const std::string& message = "");

	void Remove(const std::string& id);

	void Render(float deltaTime);

	bool HasActiveTasks();

	std::vector<TaskInfo> GetActiveTasks();

}