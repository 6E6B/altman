#pragma once

#include <string>
#include <vector>
#include <imgui.h>

struct ImGuiViewport;

namespace ButtonRightStatus {
	enum class Type {
		Idle,
		Loading,
		Success,
		Warning,
		Error
	};

	struct SelectedAccount {
		std::string label;
		ImVec4 color;
	};

	void AddText(const std::string& text, Type type = Type::Success, float duration = 8.0f);

	void Loading(const std::string& text);                          // Persistent until cleared
	void Success(const std::string& text, float duration = 8.0f);
	void Info(const std::string& text, float duration = 8.0f);
	void Warning(const std::string& text, float duration = 8.0f);
	void Error(const std::string& text, float duration = 8.0f);

	void Clear();

	void Render(const ImGuiViewport* viewport, float deltaTime,
				const std::vector<SelectedAccount>& selectedAccounts = {});
}