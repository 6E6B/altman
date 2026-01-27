#include "console/console.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <format>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

struct LogEntry {
        Console::Level level;
        std::string text;
};

static std::vector<LogEntry> g_logMessages;
static std::mutex g_logMutex;

static std::string g_latestStatusMessage = "Ready.";
static std::mutex g_statusMessageMutex;

static std::vector<LogEntry> g_pendingLogs;
static std::mutex g_queueMutex;
static std::condition_variable g_logCv;

static bool g_loggerRunning = true;
static std::thread g_loggerThread;

static char g_searchBuffer[256] = "";

static std::string getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm buf {};

#ifdef _WIN32
    localtime_s(&buf, &in_time_t);
#else
    localtime_r(&in_time_t, &buf);
#endif

    std::ostringstream ss;
    ss << std::put_time(&buf, "%H:%M:%S");
    return ss.str();
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char) std::tolower(c);
    });
    return s;
}

static const char *LevelPrefix(Console::Level level) {
    switch (level) {
        case Console::Level::Info:
            return "[INFO]";
        case Console::Level::Warn:
            return "[WARN]";
        case Console::Level::Error:
            return "[ERROR]";
    }
    return "[UNK]";
}

static void LoggerThreadFunc() {
    while (g_loggerRunning) {
        std::unique_lock<std::mutex> lock(g_queueMutex);
        g_logCv.wait(lock, [] {
            return !g_pendingLogs.empty() || !g_loggerRunning;
        });

        while (!g_pendingLogs.empty()) {
            LogEntry entry = std::move(g_pendingLogs.front());
            g_pendingLogs.erase(g_pendingLogs.begin());
            lock.unlock();

            {
                std::lock_guard<std::mutex> lg(g_logMutex);
                g_logMessages.push_back(entry);
            }
            {
                std::lock_guard<std::mutex> lg(g_statusMessageMutex);
                g_latestStatusMessage = entry.text;
            }

            lock.lock();
        }
    }
}

static struct LoggerInit {
        LoggerInit() {
            g_loggerThread = std::thread(LoggerThreadFunc);
        }
        ~LoggerInit() {
            g_loggerRunning = false;
            g_logCv.notify_all();
            if (g_loggerThread.joinable()) {
                g_loggerThread.join();
            }
        }
} s_loggerInit;

namespace Console {

    void Log(Level level, const std::string &message) {
        auto finalEntry = std::format("[{}] {} {}", getCurrentTimestamp(), LevelPrefix(level), message);

        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            g_pendingLogs.push_back({level, finalEntry});
        }
        g_logCv.notify_one();
    }

    std::string GetLatestLogMessageForStatus() {
        std::lock_guard<std::mutex> lock(g_statusMessageMutex);
        return g_latestStatusMessage;
    }

    std::vector<std::string> GetLogs() {
        std::lock_guard<std::mutex> lock(g_logMutex);
        std::vector<std::string> out;
        for (const auto &e: g_logMessages) {
            out.push_back(e.text);
        }
        return out;
    }

    void RenderConsoleTab() {
        ImGuiStyle &style = ImGui::GetStyle();

        float desiredTextIndent = style.WindowPadding.x;

        ImGui::InputTextWithHint("##SearchLog", "Search...", g_searchBuffer, IM_ARRAYSIZE(g_searchBuffer));

        ImGui::SameLine();

        if (ImGui::Button("Clear")) {
            std::lock_guard<std::mutex> lock(g_logMutex);
            g_logMessages.clear();
            g_searchBuffer[0] = '\0';
        }

        ImGui::SameLine();

        if (ImGui::Button("Copy")) {
            std::lock_guard<std::mutex> lock(g_logMutex);

            std::string allLogs;
            allLogs.reserve(8192);

            for (const auto& entry : g_logMessages) {
                allLogs += entry.text;
                allLogs += '\n';
            }

            ImGui::SetClipboardText(allLogs.c_str());
        }

        ImGui::Separator();

        ImGui::BeginChild("LogScrollingRegion", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_None);

        std::string searchLower = toLower(g_searchBuffer);

        std::lock_guard<std::mutex> lock(g_logMutex);
        for (const auto &entry: g_logMessages) {
            if (!searchLower.empty() && toLower(entry.text).find(searchLower) == std::string::npos) {
                continue;
            }

            ImVec4 color;
            switch (entry.level) {
                case Level::Info:
                    color = {0.85f, 0.85f, 0.85f, 1.0f};
                    break;
                case Level::Warn:
                    color = {1.0f, 0.85f, 0.3f, 1.0f};
                    break;
                case Level::Error:
                    color = {1.0f, 0.4f, 0.4f, 1.0f};
                    break;
            }

            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::Indent(desiredTextIndent);
            ImGui::TextUnformatted(entry.text.c_str());
            ImGui::Unindent(desiredTextIndent);
            ImGui::PopStyleColor();
        }

        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }

        ImGui::EndChild();
    }
} // namespace Console
