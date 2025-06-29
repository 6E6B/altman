#define _CRT_SECURE_NO_WARNINGS

#include <unordered_set>
#include <filesystem>
#include <imgui.h>
#include <string>
#include <system_error>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <utility>
#include <algorithm>

#include "history.h"
#include "log_types.h"
#include "log_parser.h"
#include "history_utils.h"

#include "system/threading.h"
#include "system/launcher.hpp"
#include "ui/modal_popup.h"
#include "core/status.h"
#include "ui/confirm.h"
#include "../../ui.h"
#include "../data.h"

namespace fs = filesystem;
using namespace ImGui;
using namespace std;

static int g_selected_log_idx = -1;

static auto ICON_REFRESH = "\xEF\x8B\xB1 ";
static auto ICON_TRASH = "\xEF\x87\xB8 ";

static vector<LogInfo> g_logs;
static atomic_bool g_logs_loading{false};
static atomic_bool g_stop_log_watcher{false};
static once_flag g_start_log_watcher_once;
static mutex g_logs_mtx;

static void clearLogs() {
	string dir = logsFolder();
	if (!dir.empty() && fs::exists(dir)) {
		for (const auto &entry: fs::directory_iterator(dir)) {
			if (entry.is_regular_file() && entry.path().extension() == ".log") {
				error_code ec;
				fs::remove(entry.path(), ec);
				if (ec)
					LOG_WARN("Failed to delete log: " + entry.path().string());
			}
		}
	} {
		lock_guard<mutex> lk(g_logs_mtx);
		g_logs.clear();
		g_selected_log_idx = -1;
		Data::SaveLogHistory(g_logs);
	}
}

static void refreshLogs() {
	if (g_logs_loading.load())
		return;

	g_logs_loading = true;
	Threading::newThread([]() {
		LOG_INFO("Scanning Roblox logs folder...");
		vector<LogInfo> tempLogs;
		string dir = logsFolder();
		if (!dir.empty() && fs::exists(dir)) {
			for (const auto &entry: fs::directory_iterator(dir)) {
				if (entry.is_regular_file()) {
					string fName = entry.path().filename().string();
					if (fName.length() > 4 && fName.substr(fName.length() - 4) == ".log") {
						LogInfo logInfo;
						logInfo.fileName = fName;
						logInfo.fullPath = entry.path().string();
						parseLogFile(logInfo);
						if (!logInfo.timestamp.empty() || !logInfo.version.empty()) {
							tempLogs.push_back(logInfo);
						}
					}
				}
			}
		}

		sort(tempLogs.begin(), tempLogs.end(), [](const LogInfo &a, const LogInfo &b) {
			return b.timestamp < a.timestamp;
		}); {
			lock_guard<mutex> lk(g_logs_mtx);
			for (auto &log: tempLogs) {
				auto it = find_if(g_logs.begin(), g_logs.end(), [&](const LogInfo &a) {
					return a.fileName == log.fileName;
				});
				if (it == g_logs.end())
					g_logs.push_back(log);
			}
			sort(g_logs.begin(), g_logs.end(), [](const LogInfo &a, const LogInfo &b) {
				return b.timestamp < a.timestamp;
			});
			g_selected_log_idx = -1;
			Data::SaveLogHistory(g_logs);
		}

		LOG_INFO("Log scan complete.");
		g_logs_loading = false;
	});
}

static void workerScan() {
	string dir = logsFolder();
	if (dir.empty() || !fs::exists(dir))
		return;

	vector<LogInfo> tempLogs;
	unordered_set<string> seenFiles; {
		lock_guard<mutex> lk(g_logs_mtx);
		for (const auto &log: g_logs)
			seenFiles.insert(log.fileName);
	}

	for (const auto &entry: fs::directory_iterator(dir)) {
		if (g_stop_log_watcher.load())
			return;
		if (entry.is_regular_file()) {
			string fName = entry.path().filename().string();
			if (fName.rfind(".log", fName.length() - 4) != string::npos) {
				if (seenFiles.contains(fName))
					continue;

				LogInfo logInfo;
				logInfo.fileName = fName;
				logInfo.fullPath = entry.path().string();
				parseLogFile(logInfo);
				if (!logInfo.timestamp.empty() || !logInfo.version.empty()) {
					tempLogs.push_back(logInfo);
				}
			}
		}
	}

	if (!tempLogs.empty()) {
		lock_guard<mutex> lk(g_logs_mtx);
		for (auto &log: tempLogs) {
			auto it = find_if(g_logs.begin(), g_logs.end(),
			                  [&](const LogInfo &a) { return a.fileName == log.fileName; });
			if (it == g_logs.end())
				g_logs.push_back(log);
		}
		sort(g_logs.begin(), g_logs.end(), [](const LogInfo &a, const LogInfo &b) {
			return b.timestamp < a.timestamp;
		});
		Data::SaveLogHistory(g_logs);
	}

	refreshLogs();
}

static void startLogWatcher() { {
		lock_guard<mutex> lk(g_logs_mtx);
		g_logs = Data::LoadLogHistory();
	}
	refreshLogs();
}

static void DisplayOptionalText(const char *label, const string &value) {
	if (!value.empty()) {
		PushID(label);
		Text("%s: %s", label, value.c_str());
		if (BeginPopupContextItem("CopyHistoryValue")) {
			if (MenuItem("Copy")) {
				SetClipboardText(value.c_str());
			}
			EndPopup();
		}
		PopID();
	}
}

static void DisplayLogDetails(const LogInfo &logInfo) {
	float desiredTextIndent = 8.0f;

	ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
	                             ImGuiTableFlags_SizingFixedFit;

	PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 4.0f));
	if (BeginTable("HistoryInfoTable", 2, tableFlags)) {
		TableSetupColumn("##historylabel", ImGuiTableColumnFlags_WidthFixed, 110.f);
		TableSetupColumn("##historyvalue", ImGuiTableColumnFlags_WidthStretch);

		auto addRow = [&](const char *label, const string &value) {
			if (value.empty())
				return;
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
				if (MenuItem("Copy")) {
					SetClipboardText(value.c_str());
				}
				EndPopup();
			}
			PopID();
			Spacing();
			Unindent(desiredTextIndent);
		};

		addRow("File:", logInfo.fileName);

		string timeStr = friendlyTimestamp(logInfo.timestamp);
		addRow("Time:", timeStr);
		addRow("Version:", logInfo.version);
		addRow("Channel:", logInfo.channel);
		addRow("Place ID:", logInfo.placeId);
		addRow("Job ID:", logInfo.jobId);
		addRow("Universe ID:", logInfo.universeId);
		if (!logInfo.serverIp.empty()) {
			string serverStr = logInfo.serverIp + ":" + logInfo.serverPort;
			addRow("Server:", serverStr);
		}
		addRow("User ID:", logInfo.userId);

		EndTable();
	}
	PopStyleVar();
}

void RenderHistoryTab() {
	call_once(g_start_log_watcher_once, startLogWatcher);

	if (Button((string(ICON_REFRESH) + " Refresh Logs").c_str())) {
		refreshLogs();
	}
	SameLine();
	if (Button((string(ICON_TRASH) + " Clear Logs").c_str())) {
		ConfirmPopup::Add("Clear all logs?", []() { clearLogs(); });
	}
	SameLine();
	if (g_logs_loading.load()) {
		TextUnformatted("Loading...");
	}

	Separator();

	float listWidth = GetContentRegionAvail().x * 0.4f;
	float detailWidth = GetContentRegionAvail().x * 0.6f - GetStyle().ItemSpacing.x;
	if (detailWidth <= 0)
		detailWidth = GetContentRegionAvail().x - listWidth - GetStyle().ItemSpacing.x;
	if (listWidth <= 0)
		listWidth = 200;

	BeginChild("##HistoryList", ImVec2(listWidth, 0), true); {
		lock_guard<mutex> lk(g_logs_mtx);
		string lastDay;
		string lastVersion;
		bool indented = false;

		for (int i = 0; i < static_cast<int>(g_logs.size()); ++i) {
			const auto &logInfo = g_logs[i];

			string thisDay = logInfo.timestamp.size() >= 10 ? logInfo.timestamp.substr(0, 10) : "Unknown";

			if (thisDay != lastDay) {
				if (indented)
					Unindent();
				string header = thisDay;
				SeparatorText(header.c_str());
				Indent();
				indented = true;
				lastDay = thisDay;
			}

			PushID(i);
			if (Selectable(niceLabel(logInfo).c_str(), g_selected_log_idx == i))
				g_selected_log_idx = i;
			if (!logInfo.placeId.empty() && !logInfo.jobId.empty() &&
			    BeginPopupContextItem("HistoryRowCtx")) {
				if (MenuItem("Copy Place ID")) {
					SetClipboardText(logInfo.placeId.c_str());
				}
				if (MenuItem("Copy Job ID")) {
					SetClipboardText(logInfo.jobId.c_str());
				}
				if (BeginMenu("Copy Launch Method")) {
					char buf[256];
					snprintf(buf, sizeof(buf), "roblox://placeId=%s&gameInstanceId=%s",
					         logInfo.placeId.c_str(), logInfo.jobId.c_str());
					if (MenuItem("Deep Link")) SetClipboardText(buf);
					string js = "Roblox.GameLauncher.joinGameInstance(" + logInfo.placeId + ", \"" +
					            logInfo.jobId + "\")";
					if (MenuItem("JavaScript")) SetClipboardText(js.c_str());
					string luau =
							"game:GetService(\"TeleportService\"):TeleportToPlaceInstance(" + logInfo.placeId +
							", \"" + logInfo.jobId + "\")";
					if (MenuItem("ROBLOX Luau")) SetClipboardText(luau.c_str());
					ImGui::EndMenu();
				}
				if (MenuItem("Generate Invite Link")) {
					string link = "https://www.roblox.com/games/start?placeId=" + logInfo.placeId +
					              "&gameInstanceId=" + logInfo.jobId;
					SetClipboardText(link.c_str());
				}
				EndPopup();
			}
			PopID();
		}

		if (indented)
			Unindent();
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
			DisplayLogDetails(logInfo);

			Separator();

			Indent(desiredTextIndent / 2);
			bool canLaunch = !logInfo.placeId.empty() && !logInfo.jobId.empty() &&
			                 !g_selectedAccountIds.empty();
			if (canLaunch && Button("Launch this game session")) {
				if (!logInfo.placeId.empty() && !logInfo.jobId.empty() && !g_selectedAccountIds.empty()) {
					uint64_t place_id_val = 0;
					try {
						place_id_val = stoull(logInfo.placeId);
					} catch (...) {
					}

					if (place_id_val > 0) {
						vector<pair<int, string> > accounts;
                                                for (int id: g_selectedAccountIds) {
                                                        auto it = find_if(g_accounts.begin(), g_accounts.end(),
                                                                          [&](const AccountData &a) { return a.id == id; });
                                                        if (it != g_accounts.end() && it->status != "Banned")
                                                                accounts.emplace_back(it->id, it->cookie);
                                                }
						if (!accounts.empty()) {
							LOG_INFO("Launching game from history...");
							thread([place_id_val, jobId = logInfo.jobId, accounts]() {
										launchRobloxSequential(place_id_val, jobId, accounts);
									})
									.detach();
						} else {
							LOG_INFO("Selected account not found.");
						}
					} else {
						LOG_INFO("Invalid Place ID in log.");
					}
				} else {
					LOG_INFO("Place ID missing or no account selected.");
					if (g_selectedAccountIds.empty()) {
						Status::Error("No account selected to open log entry.");
						ModalPopup::Add("Select an account first.");
					} else {
						Status::Error("Invalid log entry.");
						ModalPopup::Add("Invalid log entry.");
					}
				}
			}

			Separator();
			TextUnformatted("Raw Log Output:");
			BeginChild("##LogOutputScroll", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
			for (const auto &line: logInfo.outputLines) {
				TextUnformatted(line.c_str());
			}
			EndChild();
			Unindent(desiredTextIndent / 2);
		}
	} else {
		Indent(desiredTextIndent);
		Spacing();
		TextWrapped("Select a log from the list to see details or launch the session.");
		Unindent(desiredTextIndent);
	}
	EndChild();
}
