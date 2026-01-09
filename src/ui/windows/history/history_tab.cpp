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
#include <format>

#include "history.h"
#include "ui/windows/history/history_log_types.h"
#include "ui/windows/history/history_log_parser.h"
#include "history_utils.h"
#include "utils/time_utils.h"

#include "utils/account_utils.h"
#include "../accounts/accounts_join_ui.h"
#include "ui/widgets/context_menus.h"
#include "components/data.h"
#include "ui/widgets/bottom_right_status.h"
#include "system/roblox_launcher.h"
#include "utils/threading.h"
#include "ui/widgets/modal_popup.h"
#ifdef _WIN32
    #include <windows.h>
#endif
#include "console/console.h"

namespace {
    constexpr auto ICON_REFRESH = "\xEF\x8B\xB1 ";
    constexpr auto ICON_TRASH = "\xEF\x87\xB8 ";
    constexpr auto ICON_FOLDER = "\xEF\x81\xBB ";
    constexpr auto ICON_JOIN = "\xEF\x8B\xB6 ";

    constexpr float LIST_WIDTH_RATIO = 0.25f;
    constexpr float DETAIL_WIDTH_RATIO = 0.75f;
    constexpr float TEXT_INDENT = 8.0f;
    constexpr float MIN_LIST_WIDTH = 150.0f;

    int g_selected_log_idx = -1;
    std::vector<LogInfo> g_logs;
    std::atomic_bool g_logs_loading{false};
    std::atomic_bool g_stop_log_watcher{false};
    std::once_flag g_start_log_watcher_once;
    std::mutex g_logs_mtx;
    char g_search_buffer[128] = "";
    std::vector<int> g_filtered_log_indices;
    bool g_search_active = false;
    bool g_should_scroll_to_selection = false;
}

static void OpenFileOrFolder(const std::string& path) {
#ifdef _WIN32
    ShellExecuteA(NULL, "open", path.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif __APPLE__
    std::string command = std::format("open \"{}\"", path);
    system(command.c_str());
#else
    std::string command = std::format("xdg-open \"{}\"", path);
    system(command.c_str());
#endif
}

static void openLogsFolder() {
    std::string dir = logsFolder();
    if (!dir.empty() && std::filesystem::exists(dir)) {
        OpenFileOrFolder(dir);
    } else {
        LOG_WARN("Logs folder not found.");
    }
}

static std::string toLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return str;
}

static bool matchesSearch(const LogInfo& log, const std::string& searchTerm) {
    auto contains = [&searchTerm](const std::string& field) {
        return toLower(field).find(searchTerm) != std::string::npos;
    };

    if (contains(log.fileName) || contains(log.fullPath) ||
        contains(log.version) || contains(log.placeId) ||
        contains(log.jobId) || contains(log.universeId) ||
        contains(log.userId)) {
        return true;
    }

    for (const auto& session : log.sessions) {
        if (searchTerm.find(session.placeId) != std::string::npos ||
            searchTerm.find(session.jobId) != std::string::npos ||
            searchTerm.find(session.universeId) != std::string::npos ||
            searchTerm.find(session.serverIp) != std::string::npos) {
            return true;
        }
    }

    return false;
}

static void updateFilteredLogs() {
    g_filtered_log_indices.clear();
    g_search_active = (g_search_buffer[0] != '\0');

    if (g_logs_loading.load()) {
        g_search_buffer[0] = '\0';
        g_search_active = false;
        return;
    }

    if (!g_search_active) {
        return;
    }

    std::string searchTerm = toLower(g_search_buffer);

    std::lock_guard<std::mutex> lk(g_logs_mtx);
    for (int i = 0; i < static_cast<int>(g_logs.size()); ++i) {
        const auto& log = g_logs[i];

        if (log.isInstallerLog) {
            continue;
        }

        if (matchesSearch(log, searchTerm)) {
            g_filtered_log_indices.push_back(i);
        }
    }

    if (g_selected_log_idx != -1) {
        bool selectionInFiltered = std::any_of(
            g_filtered_log_indices.begin(),
            g_filtered_log_indices.end(),
            [](int idx) { return idx == g_selected_log_idx; }
        );

        if (!selectionInFiltered) {
            g_selected_log_idx = -1;
        }
    }
}

static void clearLogs() {
    std::string dir = logsFolder();
    if (!dir.empty() && std::filesystem::exists(dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".log") {
                std::error_code ec;
                std::filesystem::remove(entry.path(), ec);
                if (ec) {
                    LOG_WARN("Failed to delete log: {}", entry.path().string());
                }
            }
        }
    }

    std::lock_guard<std::mutex> lk(g_logs_mtx);
    g_logs.clear();
    g_selected_log_idx = -1;
}

static void refreshLogs() {
    if (g_logs_loading.load()) {
        return;
    }

    g_logs_loading = true;
    Threading::newThread([]() {
        LOG_INFO("Scanning Roblox logs folder...");
        std::vector<LogInfo> tempLogs;
        std::string dir = logsFolder();

        if (!dir.empty() && std::filesystem::exists(dir)) {
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                if (entry.is_regular_file()) {
                    std::string fName = entry.path().filename().string();
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

        std::sort(tempLogs.begin(), tempLogs.end(),
                  [](const LogInfo& a, const LogInfo& b) {
                      return b.timestamp < a.timestamp;
                  });

        {
            std::lock_guard<std::mutex> lk(g_logs_mtx);
            g_logs.clear();
            g_logs = std::move(tempLogs);
            g_selected_log_idx = -1;
        }

        LOG_INFO("Log scan complete. Recreated logs cache with {} logs.", tempLogs.size());
        g_logs_loading = false;

        updateFilteredLogs();
    });
}

static void startLogWatcher() {
    {
        std::lock_guard<std::mutex> lk(g_logs_mtx);
        g_logs.clear();
    }

    g_search_buffer[0] = '\0';
    g_search_active = false;
    g_filtered_log_indices.clear();

    refreshLogs();
}

static void addTableRow(const char* label, const std::string& value, float indent) {
    if (value.empty()) {
        return;
    }

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::Indent(indent);
    ImGui::Spacing();
    ImGui::TextUnformatted(label);
    ImGui::Spacing();
    ImGui::Unindent(indent);

    ImGui::TableSetColumnIndex(1);
    ImGui::Indent(indent);
    ImGui::Spacing();
    ImGui::PushID(label);
    ImGui::TextWrapped("%s", value.c_str());
    if (ImGui::BeginPopupContextItem("CopyHistoryValue")) {
        if (ImGui::MenuItem("Copy")) {
            ImGui::SetClipboardText(value.c_str());
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::Spacing();
    ImGui::Unindent(indent);
}

static float calculateLabelWidth(const std::vector<const char*>& labels) {
    float maxWidth = ImGui::GetFontSize() * 6.875f;
    for (const char* label : labels) {
        float width = ImGui::CalcTextSize(label).x;
        maxWidth = std::max(maxWidth, width + ImGui::GetFontSize() * 2.0f);
    }
    return maxWidth;
}

static void DisplayLogDetails(const LogInfo& logInfo) {
    ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerH |
                                 ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_SizingFixedFit;

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 4.0f));

    std::vector<const char*> labels = {
        "File:", "Time:", "Version:", "Channel:", "User ID:"
    };
    float historyLabelColumnWidth = calculateLabelWidth(labels);

    if (ImGui::BeginTable("HistoryInfoTable", 2, tableFlags)) {
        ImGui::TableSetupColumn("##historylabel", ImGuiTableColumnFlags_WidthFixed, historyLabelColumnWidth);
        ImGui::TableSetupColumn("##historyvalue", ImGuiTableColumnFlags_WidthStretch);

        addTableRow("File:", logInfo.fileName, TEXT_INDENT);

        if (ImGui::BeginPopupContextItem("LogDetailsFileContextMenu")) {
            if (ImGui::MenuItem("Copy File Name")) {
                ImGui::SetClipboardText(logInfo.fileName.c_str());
            }
            if (ImGui::MenuItem("Copy File Path")) {
                ImGui::SetClipboardText(logInfo.fullPath.c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Open File")) {
                OpenFileOrFolder(logInfo.fullPath);
            }
            ImGui::EndPopup();
        }

        std::string timeStr = friendlyTimestamp(logInfo.timestamp);
        time_t tAbs = parseIsoTimestamp(logInfo.timestamp);
        std::string timeWithRel = (tAbs ? formatAbsoluteWithRelativeLocal(tAbs) : timeStr);
        addTableRow("Time:", timeWithRel, TEXT_INDENT);
        addTableRow("Version:", logInfo.version, TEXT_INDENT);
        addTableRow("Channel:", logInfo.channel, TEXT_INDENT);
        addTableRow("User ID:", logInfo.userId, TEXT_INDENT);

        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    if (!logInfo.sessions.empty()) {
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Indent(TEXT_INDENT);
        ImGui::TextUnformatted("Game Instances:");
        ImGui::Unindent(TEXT_INDENT);
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();

        ImGuiTreeNodeFlags baseFlags = ImGuiTreeNodeFlags_DefaultOpen;
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 3.0f);

        for (size_t i = 0; i < logInfo.sessions.size(); i++) {
            const auto& session = logInfo.sessions[i];

            std::string sessionTitle = !session.timestamp.empty()
                ? friendlyTimestamp(session.timestamp)
                : std::format("Game Instance {}", i + 1);

            ImGui::PushID(static_cast<int>(i));

            ImVec4 headerColor = (i % 2 == 0)
                ? ImVec4(0.2f, 0.2f, 0.2f, 0.55f)
                : ImVec4(0.25f, 0.25f, 0.25f, 0.55f);
            ImVec4 hoverColor = (i % 2 == 0)
                ? ImVec4(0.3f, 0.3f, 0.3f, 0.55f)
                : ImVec4(0.35f, 0.35f, 0.35f, 0.55f);
            ImVec4 activeColor = (i % 2 == 0)
                ? ImVec4(0.25f, 0.25f, 0.25f, 0.55f)
                : ImVec4(0.3f, 0.3f, 0.3f, 0.55f);

            ImGui::PushStyleColor(ImGuiCol_Header, headerColor);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, hoverColor);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, activeColor);

            if (ImGui::TreeNodeEx(sessionTitle.c_str(), baseFlags)) {
                ImGui::PopStyleColor(3);

                if (ImGui::BeginTable("InstanceDetailsTable", 2, ImGuiTableFlags_BordersInnerV)) {
                    std::vector<const char*> instLabels;
                    if (!session.placeId.empty()) instLabels.push_back("Place ID:");
                    if (!session.jobId.empty()) instLabels.push_back("Job ID:");
                    if (!session.universeId.empty()) instLabels.push_back("Universe ID:");
                    if (!session.serverIp.empty()) instLabels.push_back("Server IP:");
                    if (!session.serverPort.empty()) instLabels.push_back("Server Port:");

                    float instLabelWidth = calculateLabelWidth(instLabels);
                    ImGui::TableSetupColumn("##field", ImGuiTableColumnFlags_WidthFixed, instLabelWidth);
                    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

                    auto addInstanceRow = [](const char* label, const std::string& value) {
                        if (value.empty()) return;

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(label);

                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushID(label);
                        ImGui::Indent(10.0f);
                        ImGui::TextWrapped("%s", value.c_str());
                        ImGui::Unindent(10.0f);
                        if (ImGui::BeginPopupContextItem(std::format("Copy{}", label).c_str())) {
                            if (ImGui::MenuItem("Copy")) {
                                ImGui::SetClipboardText(value.c_str());
                            }
                            ImGui::EndPopup();
                        }
                        ImGui::PopID();
                    };

                    addInstanceRow("Place ID:", session.placeId);
                    addInstanceRow("Job ID:", session.jobId);
                    addInstanceRow("Universe ID:", session.universeId);
                    addInstanceRow("Server IP:", session.serverIp);
                    addInstanceRow("Server Port:", session.serverPort);

                    ImGui::EndTable();
                }

                bool canLaunch = !session.placeId.empty() &&
                               !session.jobId.empty() &&
                               !g_selectedAccountIds.empty();

                if (canLaunch) {
                    ImGui::Spacing();
                    if (ImGui::Button(std::format("{} Launch Instance##{}", ICON_JOIN, i).c_str())) {
                        uint64_t place_id_val = 0;
                        if (session.placeId.find_first_not_of("0123456789") == std::string::npos) {
                            place_id_val = std::stoull(session.placeId);
                        }

                        if (place_id_val > 0) {
                            std::vector<std::pair<int, std::string>> accounts;
                            for (int id : g_selectedAccountIds) {
                                auto it = std::find_if(g_accounts.begin(), g_accounts.end(),
                                    [&](const AccountData& a) { return a.id == id; });
                                if (it != g_accounts.end() && AccountFilters::IsAccountUsable(*it)) {
                                    accounts.emplace_back(it->id, it->cookie);
                                }
                            }
                            if (!accounts.empty()) {
                                LOG_INFO("Launching game instance from history...");
                                std::thread([place_id_val, jobId = session.jobId, accounts]() {
                                    launchRobloxSequential(LaunchParams::gameJob(place_id_val, jobId), accounts);
                                }).detach();
                            } else {
                                LOG_INFO("Selected account not found.");
                            }
                        } else {
                            LOG_INFO("Invalid Place ID in instance.");
                        }
                    }

                    if (ImGui::BeginPopupContextItem(std::format("LaunchButtonCtx##{}", i).c_str(),
                                                     ImGuiPopupFlags_MouseButtonRight)) {
                        uint64_t pid = 0;
                        if (session.placeId.find_first_not_of("0123456789") == std::string::npos) {
                            pid = std::stoull(session.placeId);
                        }

                        StandardJoinMenuParams menu{};
                        menu.placeId = pid;
                        if (!session.universeId.empty() &&
                            session.universeId.find_first_not_of("0123456789") == std::string::npos) {
                            menu.universeId = std::stoull(session.universeId);
                        }
                        menu.jobId = session.jobId;
                        menu.onLaunchGame = [pid]() {
                            if (pid == 0 || g_selectedAccountIds.empty()) return;
                            std::vector<std::pair<int, std::string>> accounts;
                            for (int id : g_selectedAccountIds) {
                                auto it = std::find_if(g_accounts.begin(), g_accounts.end(),
                                    [&](const AccountData& a) { return a.id == id && AccountFilters::IsAccountUsable(a); });
                                if (it != g_accounts.end()) accounts.emplace_back(it->id, it->cookie);
                            }
                            if (!accounts.empty()) {
                                std::thread([pid, accounts]() {
                                    launchRobloxSequential(LaunchParams::standard(pid), accounts);
                                }).detach();
                            }
                        };
                        menu.onLaunchInstance = [pid, jid = session.jobId]() {
                            if (pid == 0 || jid.empty() || g_selectedAccountIds.empty()) return;
                            std::vector<std::pair<int, std::string>> accounts;
                            for (int id : g_selectedAccountIds) {
                                auto it = std::find_if(g_accounts.begin(), g_accounts.end(),
                                    [&](const AccountData& a) { return a.id == id && AccountFilters::IsAccountUsable(a); });
                                if (it != g_accounts.end()) accounts.emplace_back(it->id, it->cookie);
                            }
                            if (!accounts.empty()) {
                                std::thread([pid, jid, accounts]() {
                                    launchRobloxSequential(LaunchParams::gameJob(pid, jid), accounts);
                                }).detach();
                            }
                        };
                        menu.onFillGame = [pid]() { if (pid) FillJoinOptions(pid, ""); };
                        menu.onFillInstance = [pid, jid = session.jobId]() { if (pid) FillJoinOptions(pid, jid); };
                        RenderStandardJoinMenu(menu);
                        ImGui::EndPopup();
                    }
                }

                ImGui::TreePop();
            } else {
                ImGui::PopStyleColor(3);
            }

            ImGui::PopID();
            ImGui::Spacing();
        }

        ImGui::PopStyleVar();
    }
}

void RenderHistoryTab() {
    std::call_once(g_start_log_watcher_once, startLogWatcher);

    if (ImGui::Button(std::format("{} Refresh Logs", ICON_REFRESH).c_str())) {
        LOG_INFO("Recreating logs cache from scratch...");
        refreshLogs();
        g_search_buffer[0] = '\0';
        g_search_active = false;
        updateFilteredLogs();
    }
    ImGui::SameLine();
    if (ImGui::Button(std::format("{} Open Logs Folder", ICON_FOLDER).c_str())) {
        openLogsFolder();
    }
    ImGui::SameLine();
    if (ImGui::Button(std::format("{} Clear Logs", ICON_TRASH).c_str())) {
        ModalPopup::AddYesNo("Clear all logs?", []() {
            clearLogs();
            g_search_buffer[0] = '\0';
            g_search_active = false;
            updateFilteredLogs();
        });
    }
    ImGui::SameLine();
    if (g_logs_loading.load()) {
        ImGui::TextUnformatted("Loading...");
        ImGui::SameLine();
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("Search");
    ImGui::SameLine();
    float clearButtonWidth = ImGui::CalcTextSize("Clear").x + ImGui::GetStyle().FramePadding.x * 4.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x - clearButtonWidth);
    bool searchChanged = ImGui::InputText("##SearchLogs", g_search_buffer, IM_ARRAYSIZE(g_search_buffer));

    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        g_search_buffer[0] = '\0';
        searchChanged = true;
        g_should_scroll_to_selection = true;
    }

    if (searchChanged) {
        updateFilteredLogs();
    }

    if (g_search_active) {
        ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f),
            std::format("Found {} matching logs", g_filtered_log_indices.size()).c_str());
    }

    ImGui::Separator();

    float listWidth = ImGui::GetContentRegionAvail().x * LIST_WIDTH_RATIO;
    float detailWidth = ImGui::GetContentRegionAvail().x * DETAIL_WIDTH_RATIO - ImGui::GetStyle().ItemSpacing.x;
    if (detailWidth <= 0) {
        detailWidth = ImGui::GetContentRegionAvail().x - listWidth - ImGui::GetStyle().ItemSpacing.x;
    }
    if (listWidth <= 0) {
        listWidth = MIN_LIST_WIDTH;
    }

    ImGui::BeginChild("##HistoryList", ImVec2(listWidth, 0), true);
    {
        std::lock_guard<std::mutex> lk(g_logs_mtx);
        std::string lastDay;
        bool indented = false;

        const std::vector<int>& indices = g_search_active ? g_filtered_log_indices : std::vector<int>();
        int numLogsToDisplay = g_search_active ? static_cast<int>(indices.size()) : static_cast<int>(g_logs.size());

        auto getLogIndex = [&](int i) -> int {
            return g_search_active ? indices[i] : i;
        };

        if (g_should_scroll_to_selection && !g_search_active && g_selected_log_idx >= 0) {
            ImGui::SetScrollHereY();
            g_should_scroll_to_selection = false;
        }

        for (int i = 0; i < numLogsToDisplay; ++i) {
            int logIndex = getLogIndex(i);
            const auto& logInfo = g_logs[logIndex];

            if (logInfo.isInstallerLog) {
                continue;
            }

            std::string thisDay = logInfo.timestamp.size() >= 10
                ? logInfo.timestamp.substr(0, 10)
                : "Unknown";

            if (thisDay != lastDay) {
                if (indented) {
                    ImGui::Unindent();
                }
                ImGui::SeparatorText(thisDay.c_str());
                ImGui::Indent();
                indented = true;
                lastDay = thisDay;
            }

            ImGui::PushID(logIndex);
            if (ImGui::Selectable(niceLabel(logInfo).c_str(), g_selected_log_idx == logIndex)) {
                g_selected_log_idx = logIndex;
            }

            if (ImGui::BeginPopupContextItem("LogEntryContextMenu")) {
                if (ImGui::MenuItem("Copy File Name")) {
                    ImGui::SetClipboardText(logInfo.fileName.c_str());
                }
                if (ImGui::MenuItem("Copy File Path")) {
                    ImGui::SetClipboardText(logInfo.fullPath.c_str());
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Open File")) {
                    OpenFileOrFolder(logInfo.fullPath);
                }
                ImGui::EndPopup();
            }

            ImGui::PopID();
        }

        if (indented) {
            ImGui::Unindent();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##HistoryDetails", ImVec2(detailWidth, 0), true);
    ImGui::PopStyleVar();

    if (g_selected_log_idx >= 0) {
        std::lock_guard<std::mutex> lk(g_logs_mtx);
        if (g_selected_log_idx < static_cast<int>(g_logs.size())) {
            const auto& logInfo = g_logs[g_selected_log_idx];

            float contentHeight = ImGui::GetContentRegionAvail().y;
            float buttonHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2;
            float detailsHeight = contentHeight - buttonHeight;

            ImGui::BeginChild("##DetailsContent", ImVec2(0, detailsHeight), false);
            DisplayLogDetails(logInfo);
            ImGui::EndChild();

            ImGui::Separator();

            if (ImGui::Button("Open Log File")) {
                OpenFileOrFolder(logInfo.fullPath);
            }
        }
    } else {
        ImGui::Indent(TEXT_INDENT);
        ImGui::Spacing();
        ImGui::TextWrapped("Select a log from the list to see details or launch an instance.");
        ImGui::Unindent(TEXT_INDENT);
    }
    ImGui::EndChild();
}