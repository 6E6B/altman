#define _CRT_SECURE_NO_WARNINGS

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <imgui.h>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/time_utils.h"
#include "history.h"
#include "history_utils.h"
#include "log_parser.h"
#include "log_types.h"

#include "../../ui.h"
#include "../../utils/core/account_utils.h"
#include "../accounts/accounts_join_ui.h"
#include "../context_menus.h"
#include "../data.h"
#include "core/status.h"
#include "system/launcher.hpp"
#include "system/threading.h"
#include "ui/confirm.h"
#include "ui/modal_popup.h"
#include <windows.h>

namespace fs = std::filesystem;
using namespace ImGui;
using std::atomic_bool;
using std::call_once;
using std::error_code;
using std::find_if;
using std::lock_guard;
using std::mutex;
using std::once_flag;
using std::pair;
using std::sort;
using std::string;
using std::thread;
using std::to_string;
using std::transform;
using std::vector;

static int g_selected_log_idx = -1;

static auto ICON_REFRESH = "\xEF\x8B\xB1 ";
static auto ICON_TRASH = "\xEF\x87\xB8 ";
static auto ICON_FOLDER = "\xEF\x81\xBB ";
static auto ICON_JOIN = "\xEF\x8B\xB6 ";

static vector<LogInfo> g_logs;
static atomic_bool g_logs_loading {false};
static atomic_bool g_stop_log_watcher {false};
static once_flag g_start_log_watcher_once;
static mutex g_logs_mtx;
static char g_search_buffer[128] = ""; // Buffer to hold search text
static vector<int> g_filtered_log_indices; // Indices of logs that match the search
static bool g_search_active = false; // Flag to indicate if search is active
static bool g_should_scroll_to_selection = false; // Flag to auto-scroll to selection when search is cleared

static void openLogsFolder() {
	string dir = logsFolder();
	if (!dir.empty() && fs::exists(dir)) {
		ShellExecuteA(NULL, "open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL);
	} else {
		LOG_WARN("Logs folder not found.");
	}
}

static void updateFilteredLogs() {
	g_filtered_log_indices.clear();
	g_search_active = (g_search_buffer[0] != '\0');

	// Return early if logs are loading - don't apply filters during load
	if (g_logs_loading.load()) {
		g_search_buffer[0] = '\0'; // Clear search
		g_search_active = false;
		return;
	}

	if (!g_search_active) {
		return; // No search active, no need to filter
	}

	// Convert search term to lowercase for case-insensitive comparison
	string searchTerm = g_search_buffer;
	transform(searchTerm.begin(), searchTerm.end(), searchTerm.begin(), ::tolower);

	// Find logs matching the search term
	lock_guard<mutex> lk(g_logs_mtx);
	for (int i = 0; i < static_cast<int>(g_logs.size()); ++i) {
		const auto &log = g_logs[i];

		// Skip installer logs
		if (log.isInstallerLog) { continue; }

		// Convert filename to lowercase for case-insensitive comparison
		string filename = log.fileName;
		transform(filename.begin(), filename.end(), filename.begin(), ::tolower);

		// Check various fields for matches
		bool found = false;

		// Check filename
		if (filename.find(searchTerm) != string::npos) { found = true; }

		// Check full path
		string path = log.fullPath;
		transform(path.begin(), path.end(), path.begin(), ::tolower);
		if (path.find(searchTerm) != string::npos) { found = true; }

		// Check version
		string version = log.version;
		transform(version.begin(), version.end(), version.begin(), ::tolower);
		if (version.find(searchTerm) != string::npos) { found = true; }

		// Check placeId
		string placeId = log.placeId;
		if (placeId.find(searchTerm) != string::npos) { found = true; }

		// Check jobId
		string jobId = log.jobId;
		if (jobId.find(searchTerm) != string::npos) { found = true; }

		// Check universeId
		string universeId = log.universeId;
		if (universeId.find(searchTerm) != string::npos) { found = true; }

		// Check user ID
		string userId = log.userId;
		if (userId.find(searchTerm) != string::npos) { found = true; }

		// Check session data
		for (const auto &session : log.sessions) {
			if (session.placeId.find(searchTerm) != string::npos || session.jobId.find(searchTerm) != string::npos
				|| session.universeId.find(searchTerm) != string::npos
				|| session.serverIp.find(searchTerm) != string::npos) {
				found = true;
				break;
			}
		}

		if (found) { g_filtered_log_indices.push_back(i); }
	}

	// If the current selection is no longer in the filtered list, deselect it
	if (g_selected_log_idx != -1) {
		bool selectionInFiltered = false;
		for (int idx : g_filtered_log_indices) {
			if (idx == g_selected_log_idx) {
				selectionInFiltered = true;
				break;
			}
		}

		if (!selectionInFiltered) { g_selected_log_idx = -1; }
	}
}

static void clearLogs() {
	string dir = logsFolder();
	if (!dir.empty() && fs::exists(dir)) {
		for (const auto &entry : fs::directory_iterator(dir)) {
			if (entry.is_regular_file() && entry.path().extension() == ".log") {
				error_code ec;
				fs::remove(entry.path(), ec);
				if (ec) { LOG_WARN("Failed to delete log: " + entry.path().string()); }
			}
		}
	}
	{
		lock_guard<mutex> lk(g_logs_mtx);
		g_logs.clear();
		g_selected_log_idx = -1;
	}
}

static void refreshLogs() {
	if (g_logs_loading.load()) { return; }

	g_logs_loading = true;
	Threading::newThread([]() {
		LOG_INFO("Scanning Roblox logs folder...");
		vector<LogInfo> tempLogs;
		string dir = logsFolder();
		if (!dir.empty() && fs::exists(dir)) {
			for (const auto &entry : fs::directory_iterator(dir)) {
				if (entry.is_regular_file()) {
					string fName = entry.path().filename().string();
					if (fName.length() > 4 && fName.substr(fName.length() - 4) == ".log") {
						LogInfo logInfo;
						logInfo.fileName = fName;
						logInfo.fullPath = entry.path().string();
						parseLogFile(logInfo);
						if (!logInfo.timestamp.empty() || !logInfo.version.empty()) { tempLogs.push_back(logInfo); }
					}
				}
			}
		}

		sort(tempLogs.begin(), tempLogs.end(), [](const LogInfo &a, const LogInfo &b) {
			return b.timestamp < a.timestamp;
		});
		{
			lock_guard<mutex> lk(g_logs_mtx);
			g_logs.clear();
			g_logs = tempLogs;
			g_selected_log_idx = -1;
		}

		LOG_INFO("Log scan complete. Recreated logs cache with " + std::to_string(tempLogs.size()) + " logs.");
		g_logs_loading = false;

		// Update filtered logs after refresh completes
		updateFilteredLogs();
	});
}

static void workerScan() {
	// Instead of incrementally scanning for new logs,
	// simply trigger a full refresh to rebuild the entire logs cache
	refreshLogs();
}

static void startLogWatcher() {
	{
		lock_guard<mutex> lk(g_logs_mtx);
		// Clear logs instead of loading from cache - always start fresh
		g_logs.clear();
	}
	// Reset search state when starting
	g_search_buffer[0] = '\0';
	g_search_active = false;
	g_filtered_log_indices.clear();

	refreshLogs();
}

static void DisplayOptionalText(const char *label, const string &value) {
	if (!value.empty()) {
		PushID(label);
		Text("%s: %s", label, value.c_str());
		if (BeginPopupContextItem("CopyHistoryValue")) {
			if (MenuItem("Copy")) { SetClipboardText(value.c_str()); }
			EndPopup();
		}
		PopID();
	}
}

static void DisplayLogDetails(const LogInfo &logInfo) {
	float desiredTextIndent = 8.0f;

	ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;

	PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 4.0f));
	// Compute label column width for the summary table based on visible rows
	float historyLabelColumnWidth = GetFontSize() * 6.875f; // sensible minimum
	{
		vector<const char *> labels;
		labels.push_back("File:");
		labels.push_back("Time:");
		labels.push_back("Version:");
		labels.push_back("Channel:");
		labels.push_back("User ID:");
		float mx = 0.0f;
		for (const char *lbl : labels) { mx = (std::max)(mx, CalcTextSize(lbl).x); }
		historyLabelColumnWidth = (std::max)(historyLabelColumnWidth, mx + GetFontSize() + GetFontSize());
	}
	if (BeginTable("HistoryInfoTable", 2, tableFlags)) {
		TableSetupColumn("##historylabel", ImGuiTableColumnFlags_WidthFixed, historyLabelColumnWidth); // adaptive
		TableSetupColumn("##historyvalue", ImGuiTableColumnFlags_WidthStretch);

		auto addRow = [&](const char *label, const string &value) {
			if (value.empty()) { return; }
			TableNextRow();
			TableSetColumnIndex(0);
			Indent(desiredTextIndent);
			Spacing();
			TextUnformatted(label);
			Spacing();
			Unindent(desiredTextIndent);

			TableSetColumnIndex(1);
			Indent(desiredTextIndent);
			Spacing();
			PushID(label);
			TextWrapped("%s", value.c_str());
			if (BeginPopupContextItem("CopyHistoryValue")) {
				if (MenuItem("Copy")) { SetClipboardText(value.c_str()); }
				EndPopup();
			}
			PopID();
			Spacing();
			Unindent(desiredTextIndent);
		};

		// Display global log information at the top
		addRow("File:", logInfo.fileName);

		// Add options to open log file and copy name/path on file row right-click
		if (BeginPopupContextItem("LogDetailsFileContextMenu")) {
			if (MenuItem("Copy File Name")) { SetClipboardText(logInfo.fileName.c_str()); }
			if (MenuItem("Copy File Path")) { SetClipboardText(logInfo.fullPath.c_str()); }
			Separator();
			if (MenuItem("Open File")) {
				ShellExecuteA(NULL, "open", logInfo.fullPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
			}
			EndPopup();
		}

		string timeStr = friendlyTimestamp(logInfo.timestamp);
		// Add relative in info panel for richer context
		time_t tAbs = parseIsoTimestamp(logInfo.timestamp);
		string timeWithRel = (tAbs ? formatAbsoluteWithRelativeLocal(tAbs) : timeStr);
		addRow("Time:", timeWithRel);
		addRow("Version:", logInfo.version);
		addRow("Channel:", logInfo.channel);
		addRow("User ID:", logInfo.userId);

		EndTable();
	}
	PopStyleVar();

	// Display game instances section
	if (!logInfo.sessions.empty()) {
		Spacing();
		Spacing();
		Spacing();
		Separator();
		Spacing();
		Spacing();
		Spacing();
		Indent(desiredTextIndent); // Apply consistent indentation
		TextUnformatted("Game Instances:");
		Unindent(desiredTextIndent);
		Spacing();
		Spacing();
		Spacing();

		ImGuiTreeNodeFlags baseFlags = ImGuiTreeNodeFlags_DefaultOpen;

		// Display each game instance with alternating colors
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 3.0f);

		for (size_t i = 0; i < logInfo.sessions.size(); i++) {
			const auto &session = logInfo.sessions[i];

			// Create a session title with timestamp
			string sessionTitle;
			if (!session.timestamp.empty()) {
				string friendlyTime = friendlyTimestamp(session.timestamp);
				sessionTitle = friendlyTime; // Removed the refresh icon
			} else {
				sessionTitle = "Game Instance " + to_string(i + 1); // Removed the refresh icon
			}

			// Set alternating background colors for each instance
			ImGui::PushID(i);

			// Use alternating row colors
			if (i % 2 == 0) {
				ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.2f, 0.2f, 0.2f, 0.55f));
				ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.55f));
				ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.25f, 0.25f, 0.25f, 0.55f));
			} else {
				ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.25f, 0.25f, 0.25f, 0.55f));
				ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.35f, 0.35f, 0.35f, 0.55f));
				ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.3f, 0.3f, 0.3f, 0.55f));
			}

			// Create a collapsible section for each instance
			if (TreeNodeEx(sessionTitle.c_str(), baseFlags)) {
				ImGui::PopStyleColor(3);

				// Using a table for aligned fields
				if (BeginTable("InstanceDetailsTable", 2, ImGuiTableFlags_BordersInnerV)) {
					// Compute label width for per-instance rows shown below
					float instLabelWidth = GetFontSize() * 7.5f;
					{
						vector<const char *> ilabels;
						if (!session.placeId.empty()) { ilabels.push_back("Place ID:"); }
						if (!session.jobId.empty()) { ilabels.push_back("Job ID:"); }
						if (!session.universeId.empty()) { ilabels.push_back("Universe ID:"); }
						if (!session.serverIp.empty()) { ilabels.push_back("Server IP:"); }
						if (!session.serverPort.empty()) { ilabels.push_back("Server Port:"); }
						float mx = 0.0f;
						for (const char *lbl : ilabels) { mx = (std::max)(mx, CalcTextSize(lbl).x); }
						instLabelWidth = (std::max)(instLabelWidth, mx + GetFontSize() + GetFontSize());
					}
					TableSetupColumn("##field", ImGuiTableColumnFlags_WidthFixed, instLabelWidth);
					TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

					// Place ID
					if (!session.placeId.empty()) {
						TableNextRow();
						TableSetColumnIndex(0);
						TextUnformatted("Place ID:");

						TableSetColumnIndex(1);
						PushID("PlaceID");
						Indent(10.0f); // Add padding before the value
						TextWrapped("%s", session.placeId.c_str());
						Unindent(10.0f);
						if (BeginPopupContextItem("CopyPlaceID")) {
							if (MenuItem("Copy")) { SetClipboardText(session.placeId.c_str()); }
							EndPopup();
						}
						PopID();
					}

					// Job ID
					if (!session.jobId.empty()) {
						TableNextRow();
						TableSetColumnIndex(0);
						TextUnformatted("Job ID:");

						TableSetColumnIndex(1);
						PushID("JobID");
						Indent(10.0f); // Add padding before the value
						TextWrapped("%s", session.jobId.c_str());
						Unindent(10.0f);
						if (BeginPopupContextItem("CopyJobID")) {
							if (MenuItem("Copy")) { SetClipboardText(session.jobId.c_str()); }
							EndPopup();
						}
						PopID();
					}

					// Universe ID
					if (!session.universeId.empty()) {
						TableNextRow();
						TableSetColumnIndex(0);
						TextUnformatted("Universe ID:");

						TableSetColumnIndex(1);
						PushID("UniverseID");
						Indent(10.0f); // Add padding before the value
						TextWrapped("%s", session.universeId.c_str());
						Unindent(10.0f);
						if (BeginPopupContextItem("CopyUniverseID")) {
							if (MenuItem("Copy")) { SetClipboardText(session.universeId.c_str()); }
							EndPopup();
						}
						PopID();
					}

					// Server IP
					if (!session.serverIp.empty()) {
						TableNextRow();
						TableSetColumnIndex(0);
						TextUnformatted("Server IP:");

						TableSetColumnIndex(1);
						PushID("ServerIP");
						Indent(10.0f);
						TextWrapped("%s", session.serverIp.c_str());
						Unindent(10.0f);
						if (BeginPopupContextItem("CopyServerIP")) {
							if (MenuItem("Copy")) { SetClipboardText(session.serverIp.c_str()); }
							EndPopup();
						}
						PopID();
					}

					// Server Port
					if (!session.serverPort.empty()) {
						TableNextRow();
						TableSetColumnIndex(0);
						TextUnformatted("Server Port:");

						TableSetColumnIndex(1);
						PushID("ServerPort");
						Indent(10.0f);
						TextWrapped("%s", session.serverPort.c_str());
						Unindent(10.0f);
						if (BeginPopupContextItem("CopyServerPort")) {
							if (MenuItem("Copy")) { SetClipboardText(session.serverPort.c_str()); }
							EndPopup();
						}
						PopID();
					}

					EndTable();
				}

				// Launch button for this specific instance
				bool canLaunch = !session.placeId.empty() && !session.jobId.empty() && !g_selectedAccountIds.empty();
				if (canLaunch) {
					Spacing();
					if (Button((string(ICON_JOIN) + " Launch Instance##" + to_string(i)).c_str())) {
						uint64_t place_id_val = 0;
						try {
							place_id_val = stoull(session.placeId);
						} catch (...) {}

						if (place_id_val > 0) {
							vector<pair<int, string>> accounts;
							for (int id : g_selectedAccountIds) {
								auto it = find_if(g_accounts.begin(), g_accounts.end(), [&](const AccountData &a) {
									return a.id == id;
								});
								if (it != g_accounts.end() && AccountFilters::IsAccountUsable(*it)) {
									accounts.emplace_back(it->id, it->cookie);
								}
							}
							if (!accounts.empty()) {
								LOG_INFO("Launching game instance from history...");
								thread([place_id_val, jobId = session.jobId, accounts]() {
									launchRobloxSequential(place_id_val, jobId, accounts);
								}).detach();
							} else {
								LOG_INFO("Selected account not found.");
							}
						} else {
							LOG_INFO("Invalid Place ID in instance.");
						}
					}

					// Context menu for the launch button
					if (BeginPopupContextItem(
							("LaunchButtonCtx##" + to_string(i)).c_str(),
							ImGuiPopupFlags_MouseButtonRight
						)) {
						uint64_t pid = 0;
						try {
							pid = stoull(session.placeId);
						} catch (...) { pid = 0; }
						StandardJoinMenuParams menu {};
						menu.placeId = pid;
						// universeId is a string in logs; try to parse
						try {
							menu.universeId = session.universeId.empty() ? 0ULL : stoull(session.universeId);
						} catch (...) { menu.universeId = 0; }
						menu.jobId = session.jobId;
						menu.onLaunchGame = [pid]() {
							if (pid == 0 || g_selectedAccountIds.empty()) { return; }
							vector<pair<int, string>> accounts;
							for (int id : g_selectedAccountIds) {
								auto it = find_if(g_accounts.begin(), g_accounts.end(), [&](const AccountData &a) {
									return a.id == id && AccountFilters::IsAccountUsable(a);
								});
								if (it != g_accounts.end()) { accounts.emplace_back(it->id, it->cookie); }
							}
							if (!accounts.empty()) {
								thread([pid, accounts]() { launchRobloxSequential(pid, "", accounts); }).detach();
							}
						};
						menu.onLaunchInstance = [pid, jid = session.jobId]() {
							if (pid == 0 || jid.empty() || g_selectedAccountIds.empty()) { return; }
							vector<pair<int, string>> accounts;
							for (int id : g_selectedAccountIds) {
								auto it = find_if(g_accounts.begin(), g_accounts.end(), [&](const AccountData &a) {
									return a.id == id && AccountFilters::IsAccountUsable(a);
								});
								if (it != g_accounts.end()) { accounts.emplace_back(it->id, it->cookie); }
							}
							if (!accounts.empty()) {
								thread([pid, jid, accounts]() { launchRobloxSequential(pid, jid, accounts); }).detach();
							}
						};
						menu.onFillGame = [pid]() {
							if (pid) { FillJoinOptions(pid, ""); }
						};
						menu.onFillInstance = [pid, jid = session.jobId]() {
							if (pid) { FillJoinOptions(pid, jid); }
						};
						RenderStandardJoinMenu(menu);
						EndPopup();
					}
				}

				TreePop();
			} else {
				ImGui::PopStyleColor(3);
			}

			PopID();
			Spacing();
		}

		ImGui::PopStyleVar();
	}
}

void RenderHistoryTab() {
	call_once(g_start_log_watcher_once, startLogWatcher);

	if (Button((string(ICON_REFRESH) + " Refresh Logs").c_str())) {
		LOG_INFO("Recreating logs cache from scratch...");
		refreshLogs();
		// Reset search when refreshing logs
		g_search_buffer[0] = '\0';
		g_search_active = false;
		updateFilteredLogs();
	}
	SameLine();
	if (Button((string(ICON_FOLDER) + " Open Logs Folder").c_str())) { openLogsFolder(); }
	SameLine();
	if (Button((string(ICON_TRASH) + " Clear Logs").c_str())) {
		ConfirmPopup::Add("Clear all logs?", []() {
			clearLogs();
			// Reset search when clearing logs
			g_search_buffer[0] = '\0';
			g_search_active = false;
			updateFilteredLogs();
		});
	}
	SameLine();
	if (g_logs_loading.load()) {
		TextUnformatted("Loading...");
		SameLine();
	}

	// Add search box on the same line, taking all available space
	SameLine();
	TextUnformatted("Search");
	SameLine();
	// Make search box take all available width between the buttons and the clear button
	SetNextItemWidth(
		GetContentRegionAvail().x - GetStyle().ItemSpacing.x - CalcTextSize("Clear").x
		- GetStyle().FramePadding.x * 4.0f
	);
	bool searchChanged = InputText("##SearchLogs", g_search_buffer, IM_ARRAYSIZE(g_search_buffer));

	SameLine();
	if (Button("Clear")) {
		g_search_buffer[0] = '\0';
		searchChanged = true;
		g_should_scroll_to_selection = true; // Auto-scroll to selection when search is cleared
	}

	// If search text changed, update filtered logs
	if (searchChanged) { updateFilteredLogs(); }

	// Show search results count if search is active (below the search bar)
	if (g_search_active) {
		TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "Found %d matching logs", g_filtered_log_indices.size());
	}

	Separator();

	float listWidth = GetContentRegionAvail().x * 0.25f; // Reduced from 0.4f to 0.25f
	float detailWidth = GetContentRegionAvail().x * 0.75f - GetStyle().ItemSpacing.x; // Increased from 0.6f to 0.75f
	if (detailWidth <= 0) { detailWidth = GetContentRegionAvail().x - listWidth - GetStyle().ItemSpacing.x; }
	if (listWidth <= 0) {
		listWidth = GetFontSize() * 9.375f; // ~150px at 16px
	}

	BeginChild("##HistoryList", ImVec2(listWidth, 0), true);
	{
		lock_guard<mutex> lk(g_logs_mtx);
		string lastDay;
		string lastVersion;
		bool indented = false;

		// Determine which logs to display - all or filtered
		const vector<int> &indices = g_search_active ? g_filtered_log_indices : vector<int>();
		int numLogsToDisplay = g_search_active ? indices.size() : g_logs.size();

		// Helper to get the log index based on whether we're filtering or not
		auto getLogIndex = [&](int i) -> int { return g_search_active ? indices[i] : i; };

		// If auto-scroll flag is set and search is cleared, scroll to selection
		if (g_should_scroll_to_selection && !g_search_active && g_selected_log_idx >= 0) {
			SetScrollHereY();
			g_should_scroll_to_selection = false;
		}

		for (int i = 0; i < numLogsToDisplay; ++i) {
			int logIndex = getLogIndex(i);
			const auto &logInfo = g_logs[logIndex];

			// Skip installer logs
			if (logInfo.isInstallerLog) { continue; }

			string thisDay = logInfo.timestamp.size() >= 10 ? logInfo.timestamp.substr(0, 10) : "Unknown";

			if (thisDay != lastDay) {
				if (indented) { Unindent(); }
				string header = thisDay;
				SeparatorText(header.c_str());
				Indent(); // Back to default indentation
				indented = true;
				lastDay = thisDay;
			}

			PushID(logIndex);
			if (Selectable(niceLabel(logInfo).c_str(), g_selected_log_idx == logIndex)) {
				g_selected_log_idx = logIndex;
			}

			// Add right-click context menu to open and manage log file
			if (BeginPopupContextItem("LogEntryContextMenu")) {
				if (MenuItem("Copy File Name")) { SetClipboardText(logInfo.fileName.c_str()); }
				if (MenuItem("Copy File Path")) { SetClipboardText(logInfo.fullPath.c_str()); }
				Separator();
				if (MenuItem("Open File")) {
					ShellExecuteA(NULL, "open", logInfo.fullPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
				}
				EndPopup();
			}

			PopID();
		}

		if (indented) { Unindent(); }
	}
	EndChild();
	SameLine();

	float desiredTextIndent = 8.0f;

	PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	BeginChild("##HistoryDetails", ImVec2(detailWidth, 0), true);
	PopStyleVar();
	if (g_selected_log_idx >= 0) {
		lock_guard<mutex> lk(g_logs_mtx);
		if (g_selected_log_idx < static_cast<int>(g_logs.size())) {
			const auto &logInfo = g_logs[g_selected_log_idx];

			// Calculate space for buttons at bottom
			float contentHeight = GetContentRegionAvail().y;
			float buttonHeight = GetFrameHeightWithSpacing() + GetStyle().ItemSpacing.y * 2;
			float detailsHeight = contentHeight - buttonHeight;

			// Details panel in a child window
			BeginChild("##DetailsContent", ImVec2(0, detailsHeight), false);
			DisplayLogDetails(logInfo);
			EndChild();

			Separator();

			// Just show Open Log File button at the bottom - instance-specific launch buttons are in each instance
			if (Button("Open Log File")) {
				ShellExecuteA(NULL, "open", logInfo.fullPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
			}

			// Button to open log file directly if not already shown as part of another condition
		}
	} else {
		Indent(desiredTextIndent);
		Spacing();
		TextWrapped("Select a log from the list to see details or launch an instance.");
		Unindent(desiredTextIndent);
	}
	EndChild();
}
