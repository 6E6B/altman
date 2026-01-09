#pragma once

#include <deque>
#include <functional>
#include <string>

namespace ModalPopup {
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
	inline int nextId{0};

	void AddYesNo(const std::string& msg, std::function<void()> onYes, std::function<void()> onNo = nullptr);
	void AddOk(const std::string& msg, std::function<void()> onOk);
	void AddInfo(const std::string& msg);
	void AddProgress(const std::string& msg, int percentage = 0);
	void CloseProgress();
	void Clear();
	void Render();

}