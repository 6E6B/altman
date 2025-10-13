#include "console.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <imgui.h>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using namespace ImGui;
using namespace std;

static vector<string> g_logMessages;
static mutex g_logMutex;
static string g_latestStatusMessage = "Ready.";
static mutex g_statusMessageMutex;
static char g_searchBuffer[256] = "";

static string getCurrentTimestamp() {
	auto now = chrono::system_clock::now();
	auto in_time_t = chrono::system_clock::to_time_t(now);
	std::tm buf {};

	localtime_s(&buf, &in_time_t);

	ostringstream ss;
	ss << put_time(&buf, "%H:%M:%S");
	return ss.str();
}

static string toLower(string s) {
	transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
	return s;
}

namespace Console {
	void Log(const string &message_content) {
		string final_log_entry = "[" + getCurrentTimestamp() + "] " + message_content;
		{
			lock_guard<mutex> lock(g_logMutex);
			g_logMessages.push_back(final_log_entry);
		}
		{
			lock_guard<mutex> statusLock(g_statusMessageMutex);
			g_latestStatusMessage = final_log_entry;
		}
	}

	string GetLatestLogMessageForStatus() {
		lock_guard<mutex> lock(g_statusMessageMutex);
		return g_latestStatusMessage;
	}

	void RenderConsoleTab() {
		ImGuiStyle &style = GetStyle();

		float desired_text_indent = style.WindowPadding.x;
		float original_child_window_padding_y = style.WindowPadding.y;
		float original_table_cell_padding_y = style.CellPadding.y;

		float button_height = 0;
		float clearButtonWidth = CalcTextSize("Clear").x + style.FramePadding.x * 2.0f;
		float copyButtonWidth = CalcTextSize("Copy").x + style.FramePadding.x * 2.0f;
		float buttons_total_width = clearButtonWidth + copyButtonWidth + style.ItemSpacing.x;
		float searchBarWidth = GetContentRegionAvail().x - buttons_total_width - style.ItemSpacing.x;
		float minField = GetFontSize() * 6.25f; // ~100px at 16px
		if (searchBarWidth < minField) { searchBarWidth = minField; }
		PushItemWidth(searchBarWidth);
		InputTextWithHint("##SearchLog", "Search...", g_searchBuffer, IM_ARRAYSIZE(g_searchBuffer));
		PopItemWidth();
		SameLine(0, style.ItemSpacing.x);
		if (Button("Clear", ImVec2(clearButtonWidth, button_height))) {
			lock_guard<mutex> lock(g_logMutex);
			g_logMessages.clear();
			g_searchBuffer[0] = '\0';
			lock_guard<mutex> statusLock(g_statusMessageMutex);
			g_latestStatusMessage = "Log cleared.";
		}
		SameLine(0, style.ItemSpacing.x);
		if (Button("Copy", ImVec2(copyButtonWidth, button_height))) {
			string logsToCopy;
			string searchTermLower = toLower(string(g_searchBuffer));
			lock_guard<mutex> lock(g_logMutex);
			for (const auto &msg : g_logMessages) {
				if (searchTermLower.empty() || toLower(msg).find(searchTermLower) != string::npos) {
					logsToCopy += msg + "\n";
				}
			}
			if (!logsToCopy.empty()) { SetClipboardText(logsToCopy.c_str()); }
		}

		float footer_height_to_reserve = style.ItemSpacing.y;
		float childHeight = GetContentRegionAvail().y - footer_height_to_reserve;
		float minChild = GetTextLineHeightWithSpacing() * 3.0f;
		if (childHeight < minChild) { childHeight = minChild; }

		PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, original_child_window_padding_y / 2));
		BeginChild("LogScrollingRegion", ImVec2(0, childHeight), ImGuiChildFlags_Border, ImGuiWindowFlags_None);
		{
			PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, original_table_cell_padding_y));

			if (BeginTable(
					"LogTable",
					1,
					ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoPadOuterX
				)) {
				lock_guard<mutex> lock(g_logMutex);
				string searchTermLower = toLower(string(g_searchBuffer));

				for (const auto &msg : g_logMessages) {
					if (searchTermLower.empty() || toLower(msg).find(searchTermLower) != string::npos) {
						TableNextRow();
						TableNextColumn();

						Spacing();

						if (desired_text_indent > 0.0f) { Indent(desired_text_indent); }
						TextUnformatted(msg.c_str());
						if (desired_text_indent > 0.0f) { Unindent(desired_text_indent); }

						Spacing();
						Separator();
					}
				}
				EndTable();
			}
			PopStyleVar(1);
		}

		bool autoScrollDesired = true;
		bool userHasScrolled = false;
		if (GetScrollMaxY() > 0) {
			if (GetScrollY() < GetScrollMaxY() - (GetTextLineHeightWithSpacing() * 1.5f)) { userHasScrolled = true; }
		}
		if (autoScrollDesired && !userHasScrolled) {
			if (GetScrollY() >= GetScrollMaxY() - style.ItemSpacing.y || GetScrollMaxY() == 0.0f) {
				SetScrollHereY(1.0f);
			}
		}

		EndChild();
		PopStyleVar(1);
	}

	std::vector<std::string> GetLogs() {
		std::lock_guard<std::mutex> lock(g_logMutex);
		return g_logMessages;
	}
} // namespace Console
