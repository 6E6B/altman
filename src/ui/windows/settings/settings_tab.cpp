#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <imgui.h>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "settings.h"

#include "components/data.h"
#include "console/console.h"
#include "system/auto_updater.h"
#include "system/multi_instance.h"
#include "system/roblox_control.h"
#include "ui/widgets/bottom_right_status.h"
#include "ui/widgets/modal_popup.h"
#include "ui/widgets/progress_overlay.h"
#include "utils/paths.h"
#include "utils/thread_task.h"

#ifdef __APPLE__
#include "network/client_manager_macos.h"
#include "network/ipa_installer_macos.h"
#include "system/client_update_checker_macos.h"
#endif

static bool g_requestOpenConsoleModal = false;

#ifdef __APPLE__
struct EnvironmentInfo {
        std::string username;
        std::string path;
        std::uintmax_t sizeBytes = 0;
        std::filesystem::file_time_type lastAccessed;
        bool selected = false;
};

struct CleanupState {
        bool isScanning = false;
        bool isCleaning = false;
        std::vector<EnvironmentInfo> environments;
        std::uintmax_t totalSize = 0;
        std::string statusMessage;
        int unusedDaysThreshold = 30;
};

static CleanupState g_cleanupState;
static std::mutex g_cleanupMutex;

namespace {
    std::string FormatBytes(std::uintmax_t bytes) {
        constexpr std::array<const char *, 5> units = {"B", "KB", "MB", "GB", "TB"};

        if (bytes == 0) {
            return "0 B";
        }

        int unitIndex = 0;
        double size = static_cast<double>(bytes);

        while (size >= 1024.0 && unitIndex < 4) {
            size /= 1024.0;
            ++unitIndex;
        }

        if (unitIndex == 0) {
            return std::format("{} {}", bytes, units[unitIndex]);
        }
        return std::format("{:.2f} {}", size, units[unitIndex]);
    }

    std::uintmax_t CalculateDirectorySize(const std::filesystem::path &dir) {
        std::uintmax_t size = 0;
        std::error_code ec;

        for (const auto &entry: std::filesystem::recursive_directory_iterator(
                 dir,
                 std::filesystem::directory_options::skip_permission_denied,
                 ec
             )) {
            if (entry.is_regular_file(ec)) {
                size += entry.file_size(ec);
            }
        }

        return size;
    }

    std::filesystem::file_time_type GetLastAccessedTime(const std::filesystem::path &dir) {
        std::filesystem::file_time_type latest = std::filesystem::file_time_type::min();
        std::error_code ec;

        for (const auto &entry: std::filesystem::recursive_directory_iterator(
                 dir,
                 std::filesystem::directory_options::skip_permission_denied,
                 ec
             )) {
            if (entry.is_regular_file(ec)) {
                auto modTime = entry.last_write_time(ec);
                if (!ec && modTime > latest) {
                    latest = modTime;
                }
            }
        }

        return latest;
    }

    int DaysSinceLastAccess(const std::filesystem::file_time_type &lastAccess) {
        if (lastAccess == std::filesystem::file_time_type::min()) {
            return std::numeric_limits<int>::max();
        }

        const auto now = std::filesystem::file_time_type::clock::now();
        const auto duration = now - lastAccess;
        const auto hours = std::chrono::duration_cast<std::chrono::hours>(duration).count();
        return static_cast<int>(hours / 24);
    }

    void ScanEnvironments() {
        const auto appDataDir = AltMan::Paths::AppData();
        if (appDataDir.empty()) {
            std::lock_guard<std::mutex> lock(g_cleanupMutex);
            g_cleanupState.statusMessage = "Failed to get AppData directory";
            g_cleanupState.isScanning = false;
            return;
        }

        const std::filesystem::path envBasePath = appDataDir / "environments";

        std::error_code ec;
        if (!std::filesystem::exists(envBasePath, ec)) {
            std::lock_guard<std::mutex> lock(g_cleanupMutex);
            g_cleanupState.environments.clear();
            g_cleanupState.totalSize = 0;
            g_cleanupState.statusMessage = "No environments folder found";
            g_cleanupState.isScanning = false;
            return;
        }

        std::vector<EnvironmentInfo> environments;
        std::uintmax_t totalSize = 0;

        for (const auto &entry: std::filesystem::directory_iterator(envBasePath, ec)) {
            if (!entry.is_directory(ec)) {
                continue;
            }

            EnvironmentInfo info;
            info.username = entry.path().filename().string();
            info.path = entry.path().string();
            info.sizeBytes = CalculateDirectorySize(entry.path());
            info.lastAccessed = GetLastAccessedTime(entry.path());
            info.selected = false;

            totalSize += info.sizeBytes;
            environments.push_back(std::move(info));
        }

        std::sort(environments.begin(), environments.end(), [](const EnvironmentInfo &a, const EnvironmentInfo &b) {
            return a.sizeBytes > b.sizeBytes;
        });

        {
            std::lock_guard<std::mutex> lock(g_cleanupMutex);
            g_cleanupState.environments = std::move(environments);
            g_cleanupState.totalSize = totalSize;
            g_cleanupState.statusMessage
                = std::format("Found {} environments ({})", g_cleanupState.environments.size(), FormatBytes(totalSize));
            g_cleanupState.isScanning = false;
        }

        LOG_INFO("Scanned {} environments, total size: {}", g_cleanupState.environments.size(), FormatBytes(totalSize));
    }

    bool DeleteEnvironment(const std::string &path, const std::string &username) {
        std::error_code ec;

        if (!std::filesystem::exists(path, ec)) {
            LOG_WARN("Environment folder does not exist: {}", path);
            return true;
        }

        const auto removed = std::filesystem::remove_all(path, ec);

        if (ec) {
            LOG_ERROR("Failed to remove environment for {}: {}", username, ec.message());
            return false;
        }

        LOG_INFO("Removed environment for {} ({} items)", username, removed);
        return true;
    }

    void CleanSelectedEnvironments(
        const std::vector<std::string> &pathsToClean,
        const std::vector<std::string> &usernames
    ) {
        {
            std::lock_guard<std::mutex> lock(g_cleanupMutex);
            g_cleanupState.isCleaning = true;
            g_cleanupState.statusMessage = "Cleaning environments...";
        }

        int successCount = 0;
        int failCount = 0;
        std::uintmax_t freedBytes = 0;

        for (size_t i = 0; i < pathsToClean.size(); ++i) {
            const auto &path = pathsToClean[i];
            const auto &username = usernames[i];

            const auto sizeBytes = CalculateDirectorySize(path);

            if (DeleteEnvironment(path, username)) {
                ++successCount;
                freedBytes += sizeBytes;
            } else {
                ++failCount;
            }

            {
                std::lock_guard<std::mutex> lock(g_cleanupMutex);
                g_cleanupState.statusMessage = std::format("Cleaning... ({}/{})", i + 1, pathsToClean.size());
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_cleanupMutex);
            g_cleanupState.isCleaning = false;

            if (failCount == 0) {
                g_cleanupState.statusMessage
                    = std::format("Cleaned {} environments, freed {}", successCount, FormatBytes(freedBytes));
            } else {
                g_cleanupState.statusMessage
                    = std::format("Cleaned {}, failed {} (freed {})", successCount, failCount, FormatBytes(freedBytes));
            }
        }

        ThreadTask::fireAndForget([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            ScanEnvironments();
        });
    }

    void OpenAccountDocumentsFolder(const AccountData &acc) {
        const auto appDataDir = AltMan::Paths::AppData();
        if (appDataDir.empty()) {
            return;
        }

        const std::string documentsPath
            = std::format("{}/environments/{}/Documents", appDataDir.string(), acc.username);

        std::error_code ec;
        if (!std::filesystem::exists(documentsPath, ec)) {
            LOG_INFO("Documents folder does not exist: {}", documentsPath);
            return;
        }

        const std::string command = std::format("open \"{}\"", documentsPath);
        const int result = std::system(command.c_str());

        if (result == 0) {
            LOG_INFO("Opened documents folder for: {}", acc.username);
        } else {
            LOG_ERROR("Failed to open documents folder for: {}", acc.username);
        }
    }

    void OpenAccountEnvironmentFolder(const AccountData &acc) {
        const auto appDataDir = AltMan::Paths::AppData();
        if (appDataDir.empty()) {
            return;
        }

        const std::string envPath = std::format("{}/environments/{}", appDataDir.string(), acc.username);

        std::error_code ec;
        if (!std::filesystem::exists(envPath, ec)) {
            LOG_INFO("Environment folder does not exist: {}", envPath);
            return;
        }

        const std::string command = std::format("open \"{}\"", envPath);
        const int result = std::system(command.c_str());

        if (result == 0) {
            LOG_INFO("Opened environment folder for: {}", acc.username);
        } else {
            LOG_ERROR("Failed to open environment folder for: {}", acc.username);
        }
    }

    void CleanAccountEnvironment(const AccountData &acc) {
        const auto appDataDir = AltMan::Paths::AppData();
        if (appDataDir.empty()) {
            return;
        }

        const std::string envPath = std::format("{}/environments/{}", appDataDir.string(), acc.username);

        std::error_code ec;
        if (!std::filesystem::exists(envPath, ec)) {
            LOG_INFO("Environment folder does not exist for: {}", acc.username);
            return;
        }

        const auto sizeBytes = CalculateDirectorySize(envPath);

        if (DeleteEnvironment(envPath, acc.username)) {
            LOG_INFO("Cleaned environment for {}, freed {}", acc.username, FormatBytes(sizeBytes));
        }
    }

    template<typename Fn> void ProcessSelectedAccounts(Fn &&operation) {
        for (const int accountId: g_selectedAccountIds) {
            if (AccountData *acc = getAccountById(accountId)) {
                operation(*acc);
            }
        }
    }

    void RenderClientSelector() {
        const auto &availableClientsForUI = MultiInstance::getAvailableClientsForUI(false);

        if (availableClientsForUI.empty()) {
            ImGui::TextDisabled("No clients available");
            ImGui::SameLine();
            if (ImGui::Button("Install Clients")) {
                ImGui::SetScrollHereY(1.0f);
            }
            return;
        }

        for (const int accountId: g_selectedAccountIds) {
            AccountData *acc = getAccountById(accountId);
            if (!acc) {
                continue;
            }

            std::string currentBase = acc->customClientBase.empty() ? "Default" : acc->customClientBase;

            ImGui::PushID(acc->id);

            ImGui::Text("%s:", acc->username.c_str());
            ImGui::SameLine();

            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::BeginCombo("##ClientSelect", currentBase.c_str())) {
                for (const auto &clientName: availableClientsForUI) {
                    const bool isInstalled = MultiInstance::isBaseClientInstalled(clientName);

                    if (!isInstalled && clientName != "Default") {
                        ImGui::BeginDisabled();
                    }

                    bool isSelected = (currentBase == clientName);

                    if (ImGui::Selectable(clientName.c_str(), isSelected)) {
                        acc->customClientBase = (clientName == "Default") ? "" : clientName;
                        Data::SaveAccounts();
                        LOG_INFO("Set {} to use base client: {}", acc->username, clientName);
                    }

                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }

                    if (!isInstalled && clientName != "Default") {
                        ImGui::EndDisabled();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::SameLine();
            std::string userClientName = "Roblox_" + acc->username;
            bool userCopyExists = MultiInstance::isClientInstalled(acc->username, userClientName);

            if (userCopyExists) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Ready");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Will copy on launch");
            }

            ImGui::PopID();
        }
    }

    void RenderEnvironmentCleanupSection() {
        ImGui::SeparatorText("Environment Cleanup");

        {
            std::lock_guard<std::mutex> lock(g_cleanupMutex);

            const bool disableButtons = g_cleanupState.isScanning || g_cleanupState.isCleaning;

            if (disableButtons) {
                ImGui::BeginDisabled();
            }

            if (ImGui::Button("Scan Environments")) {
                g_cleanupState.isScanning = true;
                g_cleanupState.statusMessage = "Scanning...";
                ThreadTask::fireAndForget(ScanEnvironments);
            }

            if (disableButtons) {
                ImGui::EndDisabled();
            }

            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Environments store per-account Roblox data including:\n"
                    "- Documents folder\n"
                    "- Cache files\n"
                    "- Client copies\n\n"
                    "Cleaning an environment will remove all this data.\n"
                    "The environment will be recreated on next launch."
                );
            }

            if (!g_cleanupState.statusMessage.empty()) {
                ImGui::SameLine();
                ImGui::TextWrapped("%s", g_cleanupState.statusMessage.c_str());
            }

            if (g_cleanupState.isScanning) {
                ImGui::SameLine();
                ImGui::TextDisabled("Scanning...");
            }
        }

        std::lock_guard<std::mutex> lock(g_cleanupMutex);

        if (g_cleanupState.environments.empty() && !g_cleanupState.isScanning) {
            ImGui::TextDisabled("No environments found. Click 'Scan Environments' to search.");
            return;
        }

        if (g_cleanupState.isScanning) {
            return;
        }

        ImGui::Spacing();

        const bool disableActions = g_cleanupState.isCleaning;

        if (disableActions) {
            ImGui::BeginDisabled();
        }

        if (ImGui::Button("Clean All Environments")) {
            const std::uintmax_t totalSize = g_cleanupState.totalSize;
            const size_t envCount = g_cleanupState.environments.size();

            ModalPopup::AddYesNo(
                std::format(
                    "Are you sure you want to clean ALL {} environments?\n\n"
                    "This will free approximately {}.\n\n"
                    "All environment data will be deleted and recreated on next launch.",
                    envCount,
                    FormatBytes(totalSize)
                ),
                []() {
                    std::vector<std::string> paths;
                    std::vector<std::string> usernames;

                    {
                        std::lock_guard<std::mutex> lock(g_cleanupMutex);
                        for (const auto &env: g_cleanupState.environments) {
                            paths.push_back(env.path);
                            usernames.push_back(env.username);
                        }
                    }

                    ThreadTask::fireAndForget([paths = std::move(paths), usernames = std::move(usernames)]() mutable {
                        CleanSelectedEnvironments(paths, usernames);
                    });
                }
            );
        }

        ImGui::SameLine();

        ImGui::SetNextItemWidth(100.0f);
        ImGui::InputInt("##UnusedDays", &g_cleanupState.unusedDaysThreshold);
        g_cleanupState.unusedDaysThreshold = std::max(1, g_cleanupState.unusedDaysThreshold);

        ImGui::SameLine();
        ImGui::Text("days");

        ImGui::SameLine();

        int unusedCount = 0;
        std::uintmax_t unusedSize = 0;
        for (const auto &env: g_cleanupState.environments) {
            const int daysSince = DaysSinceLastAccess(env.lastAccessed);
            if (daysSince >= g_cleanupState.unusedDaysThreshold) {
                ++unusedCount;
                unusedSize += env.sizeBytes;
            }
        }

        const std::string cleanUnusedLabel = std::format("Clean Unused ({})", unusedCount);

        if (unusedCount == 0) {
            ImGui::BeginDisabled();
        }

        if (ImGui::Button(cleanUnusedLabel.c_str())) {
            ModalPopup::AddYesNo(
                std::format(
                    "Clean {} environments not used in the last {} days?\n\n"
                    "This will free approximately {}.",
                    unusedCount,
                    g_cleanupState.unusedDaysThreshold,
                    FormatBytes(unusedSize)
                ),
                [threshold = g_cleanupState.unusedDaysThreshold]() {
                    std::vector<std::string> paths;
                    std::vector<std::string> usernames;

                    {
                        std::lock_guard<std::mutex> lock(g_cleanupMutex);
                        for (const auto &env: g_cleanupState.environments) {
                            const int daysSince = DaysSinceLastAccess(env.lastAccessed);
                            if (daysSince >= threshold) {
                                paths.push_back(env.path);
                                usernames.push_back(env.username);
                            }
                        }
                    }

                    ThreadTask::fireAndForget([paths = std::move(paths), usernames = std::move(usernames)]() mutable {
                        CleanSelectedEnvironments(paths, usernames);
                    });
                }
            );
        }

        if (unusedCount == 0) {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();

        int selectedCount = 0;
        std::uintmax_t selectedSize = 0;
        for (const auto &env: g_cleanupState.environments) {
            if (env.selected) {
                ++selectedCount;
                selectedSize += env.sizeBytes;
            }
        }

        const std::string cleanSelectedLabel = std::format("Clean Selected ({})", selectedCount);

        if (selectedCount == 0) {
            ImGui::BeginDisabled();
        }

        if (ImGui::Button(cleanSelectedLabel.c_str())) {
            ModalPopup::AddYesNo(
                std::format(
                    "Clean {} selected environments?\n\n"
                    "This will free approximately {}.",
                    selectedCount,
                    FormatBytes(selectedSize)
                ),
                []() {
                    std::vector<std::string> paths;
                    std::vector<std::string> usernames;

                    {
                        std::lock_guard<std::mutex> lock(g_cleanupMutex);
                        for (const auto &env: g_cleanupState.environments) {
                            if (env.selected) {
                                paths.push_back(env.path);
                                usernames.push_back(env.username);
                            }
                        }
                    }

                    ThreadTask::fireAndForget([paths = std::move(paths), usernames = std::move(usernames)]() mutable {
                        CleanSelectedEnvironments(paths, usernames);
                    });
                }
            );
        }

        if (selectedCount == 0) {
            ImGui::EndDisabled();
        }

        if (disableActions) {
            ImGui::EndDisabled();
        }

        ImGui::Spacing();

        const float availHeight = ImGui::GetContentRegionAvail().y;
        constexpr float MIN_TABLE_HEIGHT = 150.0f;
        const float tableHeight = std::max(MIN_TABLE_HEIGHT, availHeight - 50.0f);

        if (ImGui::BeginTable(
                "EnvironmentTable",
                5,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY
                    | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable,
                ImVec2(0, tableHeight)
            )) {

            ImGui::TableSetupColumn("##Select", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 30.0f);
            ImGui::TableSetupColumn("Account", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(
                "Size",
                ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort
                    | ImGuiTableColumnFlags_PreferSortDescending,
                100.0f
            );
            ImGui::TableSetupColumn("Last Used", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 70.0f);
            ImGui::TableHeadersRow();

            if (ImGuiTableSortSpecs *sortSpecs = ImGui::TableGetSortSpecs()) {
                if (sortSpecs->SpecsDirty && sortSpecs->SpecsCount > 0) {
                    const auto &spec = sortSpecs->Specs[0];
                    const bool ascending = (spec.SortDirection == ImGuiSortDirection_Ascending);

                    std::sort(
                        g_cleanupState.environments.begin(),
                        g_cleanupState.environments.end(),
                        [&spec, ascending](const EnvironmentInfo &a, const EnvironmentInfo &b) {
                            bool result = false;
                            switch (spec.ColumnIndex) {
                                case 1: // Account name
                                    result = a.username < b.username;
                                    break;
                                case 2: // Size
                                    result = a.sizeBytes < b.sizeBytes;
                                    break;
                                case 3: // Last used
                                    result = a.lastAccessed < b.lastAccessed;
                                    break;
                                default:
                                    result = a.sizeBytes < b.sizeBytes;
                                    break;
                            }
                            return ascending ? result : !result;
                        }
                    );

                    sortSpecs->SpecsDirty = false;
                }
            }

            for (auto &env: g_cleanupState.environments) {
                ImGui::TableNextRow();
                ImGui::PushID(env.path.c_str());

                ImGui::TableNextColumn();
                ImGui::Checkbox("##Select", &env.selected);

                ImGui::TableNextColumn();
                ImGui::Text("%s", env.username.c_str());

                ImGui::TableNextColumn();
                const std::string sizeStr = FormatBytes(env.sizeBytes);

                if (env.sizeBytes > 1024ULL * 1024ULL * 1024ULL) { // > 1 GB
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", sizeStr.c_str());
                } else if (env.sizeBytes > 500ULL * 1024ULL * 1024ULL) { // > 500 MB
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", sizeStr.c_str());
                } else {
                    ImGui::Text("%s", sizeStr.c_str());
                }

                ImGui::TableNextColumn();
                const int daysSince = DaysSinceLastAccess(env.lastAccessed);

                if (daysSince == std::numeric_limits<int>::max()) {
                    ImGui::TextDisabled("Unknown");
                } else if (daysSince == 0) {
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Today");
                } else if (daysSince == 1) {
                    ImGui::Text("Yesterday");
                } else if (daysSince < 7) {
                    ImGui::Text("%d days ago", daysSince);
                } else if (daysSince < 30) {
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%d days ago", daysSince);
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%d days ago", daysSince);
                }

                ImGui::TableNextColumn();

                if (g_cleanupState.isCleaning) {
                    ImGui::BeginDisabled();
                }

                if (ImGui::Button("Clean", ImVec2(-FLT_MIN, 0))) {
                    const std::string path = env.path;
                    const std::string username = env.username;
                    const std::uintmax_t size = env.sizeBytes;

                    ModalPopup::AddYesNo(
                        std::format(
                            "Clean environment for {}?\n\n"
                            "This will free {}.",
                            username,
                            FormatBytes(size)
                        ),
                        [path, username]() {
                            ThreadTask::fireAndForget([path, username]() {
                                CleanSelectedEnvironments({path}, {username});
                            });
                        }
                    );
                }

                if (g_cleanupState.isCleaning) {
                    ImGui::EndDisabled();
                }

                ImGui::PopID();
            }

            ImGui::EndTable();
        }

        ImGui::Text(
            "Total: %zu environments, %s",
            g_cleanupState.environments.size(),
            FormatBytes(g_cleanupState.totalSize).c_str()
        );

        if (g_cleanupState.isCleaning) {
            ImGui::SameLine();
            ImGui::TextDisabled("Cleaning...");
        }
    }
} // namespace
#endif // __APPLE__

void RenderSettingsTab() {
    ImGui::BeginChild(
        "SettingsScrollRegion",
        ImVec2(0, 0),
        ImGuiChildFlags_None,
        ImGuiWindowFlags_AlwaysVerticalScrollbar
    );

    if (ImGui::Button("Open Console")) {
        g_requestOpenConsoleModal = true;
    }
    ImGui::Spacing();

    ImGui::SeparatorText("Updates");

    bool checkUpdates = g_checkUpdatesOnStartup;
    if (ImGui::Checkbox("Check for updates on startup", &checkUpdates)) {
        g_checkUpdatesOnStartup = checkUpdates;
        Data::SaveSettings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Check For Updates Now")) {
        AutoUpdater::CheckForUpdates(false);
#ifdef __APPLE__
        ClientUpdateChecker::UpdateChecker::CheckAllNow();
#endif
    }

    ImGui::SameLine();
    if (ImGui::Button("Rollback to previous version")) {
        AutoUpdater::RollbackToPreviousVersion();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cleanup old backups")) {
        AutoUpdater::CleanupOldBackups();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Launch Options");

    bool multi = g_multiRobloxEnabled;
    if (ImGui::Checkbox("Multi Roblox", &multi)) {
#ifdef _WIN32
        if (multi && RobloxControl::IsRobloxRunning()) {
            ModalPopup::AddYesNo(
                "Enabling Multi Roblox requires closing all running Roblox instances.\n\n"
                "Do you want to continue?",
                []() {
                    BottomRightStatus::Loading("Waiting for Roblox to close");
                    RobloxControl::KillRobloxProcesses();
                    ThreadTask::fireAndForget([] {
                        constexpr int maxAttempts = 50;
                        constexpr int delayMs = 100;

                        for (int i = 0; i < maxAttempts; ++i) {
                            if (!RobloxControl::IsRobloxRunning()) {
                                BottomRightStatus::Clear();
                                g_multiRobloxEnabled = true;
                                MultiInstance::Enable();
                                Data::SaveSettings();
                                LOG_INFO("Multi Roblox enabled after Roblox exit");
                                return;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                        }
                        LOG_ERROR("Timed out waiting for Roblox to exit");
                    });
                }
            );
        } else {
            g_multiRobloxEnabled = multi;
            if (g_multiRobloxEnabled) {
                MultiInstance::Enable();
            } else {
                MultiInstance::Disable();
            }
            Data::SaveSettings();
        }
#else
        // Mutex does not work MacOS but we save the option anyways
        g_multiRobloxEnabled = multi;
        Data::SaveSettings();
#endif
    }

#ifdef _WIN32
    if (ImGui::IsItemHovered()) {
        if (g_multiRobloxEnabled) {
            ImGui::SetTooltip("AltMan must be running before launching Roblox for multi-instance to work.");
        } else {
            ImGui::SetTooltip("Enabling this will close any running Roblox instances.");
        }
    }
#endif

    ImGui::BeginDisabled(g_multiRobloxEnabled);

    bool killOnLaunch = g_killRobloxOnLaunch;
    if (ImGui::Checkbox("Kill Roblox When Launching", &killOnLaunch)) {
        g_killRobloxOnLaunch = killOnLaunch;
        Data::SaveSettings();
    }

    bool clearOnLaunch = g_clearCacheOnLaunch;
    if (ImGui::Checkbox("Clear Roblox Cache When Launching", &clearOnLaunch)) {
        g_clearCacheOnLaunch = clearOnLaunch;
        Data::SaveSettings();
    }

    ImGui::EndDisabled();

    static bool lowFPS = false;
    if (ImGui::Checkbox("Low FPS", &lowFPS)) {
        // roblox-player.exe --fps=60
    }
    static bool noSound = false;
    if (ImGui::Checkbox("No Sound", &noSound)) {
        // roblox-player.exe --no-sound"
    }

    ImGui::Spacing();

    if (!g_accounts.empty()) {
        ImGui::SeparatorText("Accounts");

        int interval = g_statusRefreshInterval;
        if (ImGui::InputInt("Status Refresh Interval (min)", &interval)) {
            interval = std::max(1, interval);
            if (interval != g_statusRefreshInterval) {
                g_statusRefreshInterval = interval;
                Data::SaveSettings();
            }
        }

        ImGui::Text("Default Account:");

        std::vector<std::string> accountLabels;
        std::vector<const char *> names;
        std::vector<size_t> idxMap;

        accountLabels.reserve(g_accounts.size());
        names.reserve(g_accounts.size());
        idxMap.reserve(g_accounts.size());

        int current_default_idx = -1;

        for (size_t i = 0; i < g_accounts.size(); ++i) {
            const auto &acc = g_accounts[i];
            const std::string label = (acc.displayName == acc.username)
                                          ? acc.displayName
                                          : std::format("{} ({})", acc.displayName, acc.username);

            accountLabels.push_back(label);
            names.push_back(accountLabels.back().c_str());
            idxMap.push_back(i);

            if (acc.id == g_defaultAccountId) {
                current_default_idx = static_cast<int>(names.size() - 1);
            }
        }

        int combo_idx = current_default_idx;

        if (!names.empty()) {
            if (ImGui::Combo("##defaultAccountCombo", &combo_idx, names.data(), static_cast<int>(names.size()))) {
                if (combo_idx >= 0 && combo_idx < static_cast<int>(idxMap.size())) {
                    g_defaultAccountId = g_accounts[idxMap[combo_idx]].id;
                    g_selectedAccountIds.clear();
                    g_selectedAccountIds.insert(g_defaultAccountId);
                    Data::SaveSettings();
                }
            }
        } else {
            ImGui::TextDisabled("No accounts available.");
        }
    } else {
        ImGui::TextDisabled("No accounts available to set a default.");
    }

    ImGui::Spacing();

#ifdef __APPLE__
    ImGui::SeparatorText("Selected Account Settings");

    if (g_selectedAccountIds.empty()) {
        ImGui::TextDisabled("Select accounts from the Accounts tab to configure");
    } else {
        if (ImGui::CollapsingHeader("Folders", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Button("Open Documents Folder")) {
                ProcessSelectedAccounts(OpenAccountDocumentsFolder);
            }

            ImGui::SameLine();

            if (ImGui::Button("Open Environment Folder")) {
                ProcessSelectedAccounts(OpenAccountEnvironmentFolder);
            }

            ImGui::SameLine();

            if (ImGui::Button("Clean Environment")) {
                std::vector<std::pair<std::string, std::string>> accountsToClean; // username, path
                std::uintmax_t totalSize = 0;

                const auto appDataDir = AltMan::Paths::AppData();
                if (!appDataDir.empty()) {
                    for (const int accountId: g_selectedAccountIds) {
                        if (const AccountData *acc = getAccountById(accountId)) {
                            const std::string envPath
                                = std::format("{}/environments/{}", appDataDir.string(), acc->username);

                            std::error_code ec;
                            if (std::filesystem::exists(envPath, ec)) {
                                const auto size = CalculateDirectorySize(envPath);
                                totalSize += size;
                                accountsToClean.emplace_back(acc->username, envPath);
                            }
                        }
                    }
                }

                if (accountsToClean.empty()) {
                    ModalPopup::AddInfo("No environment folders exist for the selected accounts.");
                } else {
                    std::string accountList;
                    for (const auto &[username, _]: accountsToClean) {
                        accountList += std::format("  - {}\n", username);
                    }

                    ModalPopup::AddYesNo(
                        std::format(
                            "Clean environment for {} account(s)?\n\n"
                            "{}\n"
                            "This will free approximately {}.\n\n"
                            "Environment data will be recreated on next launch.",
                            accountsToClean.size(),
                            accountList,
                            FormatBytes(totalSize)
                        ),
                        [accountsToClean]() {
                            std::vector<std::string> paths;
                            std::vector<std::string> usernames;

                            for (const auto &[username, path]: accountsToClean) {
                                paths.push_back(path);
                                usernames.push_back(username);
                            }

                            ThreadTask::fireAndForget([paths = std::move(paths),
                                                       usernames = std::move(usernames)]() mutable {
                                CleanSelectedEnvironments(paths, usernames);
                            });
                        }
                    );
                }
            }

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Remove environment data for selected accounts to free disk space.\n"
                    "The environment will be recreated on next launch."
                );
            }
        }

        ImGui::Spacing();

        if (ImGui::CollapsingHeader("Client Configuration", ImGuiTreeNodeFlags_DefaultOpen)) {
            RenderClientSelector();
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Client Management");

    bool forceLatest = g_forceLatestRobloxVersion;
    if (ImGui::Checkbox("Force Latest Roblox Version For Clients", &forceLatest)) {
        g_forceLatestRobloxVersion = forceLatest;
        Data::SaveSettings();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "If your client crashed on startup try enabling this option, then remove and install client again.\nWhen enabled, ignores client-recommended versions and always uses the latest Roblox version.\nMay cause compatibility issues with some clients.\nDefault client remains non affected."
        );
    }

    const float availHeight = ImGui::GetContentRegionAvail().y;
    constexpr float MIN_TABLE_HEIGHT = 150.0f;
    const float tableHeight = std::max(MIN_TABLE_HEIGHT, availHeight - 50.0f);

    if (ImGui::BeginTable(
            "ClientTable",
            4,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
            ImVec2(0, tableHeight)
        )) {
        ImGui::TableSetupColumn("Client", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        // Static map to store buffers per client - initialized once
        static std::unordered_map<std::string, std::array<char, 256>> keyBuffers;
        static std::unordered_map<std::string, bool> buffersInitialized;

        for (const auto &clientName: g_availableClientsNames) {
            const bool isInstalled = MultiInstance::isBaseClientInstalled(clientName);
            const bool needsKey = (clientName != "Default");

            ImGui::TableNextRow();
            ImGui::PushID(clientName.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%s", clientName.c_str());

            ImGui::TableNextColumn();
            if (needsKey) {
                auto &key = g_clientKeys[clientName];

                // Initialize buffer only once per client
                if (!buffersInitialized[clientName]) {
                    auto &buffer = keyBuffers[clientName];
                    std::strncpy(buffer.data(), key.c_str(), buffer.size() - 1);
                    buffer.back() = '\0';
                    buffersInitialized[clientName] = true;
                }

                auto &buffer = keyBuffers[clientName];

                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputText("##Key", buffer.data(), buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
                    key = std::string(buffer.data());
                    Data::SaveSettings();
                    LOG_INFO("Updated key for {}", clientName);
                }

                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    key = std::string(buffer.data());
                    Data::SaveSettings();
                    LOG_INFO("Updated key for {}", clientName);
                }
            } else {
                ImGui::TextDisabled("No key required");
            }

            ImGui::TableNextColumn();
            if (isInstalled) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Installed");
            } else {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Not Installed");
            }

            ImGui::TableNextColumn();

            const bool disableInstall = needsKey && g_clientKeys[clientName].empty();

            const bool isCurrentlyInstalling = ProgressOverlay::HasTask("client_" + clientName);
            const bool disableButton = isCurrentlyInstalling || disableInstall;

            if (disableButton) {
                ImGui::BeginDisabled();
            }

            if (isInstalled) {
                if (ImGui::Button("Remove", ImVec2(-FLT_MIN, 0))) {
                    ProgressOverlay::Add("client_" + clientName, std::format("Removing {}...", clientName));

                    ClientManager::RemoveClientAsync(
                        clientName,
                        [clientName](bool success, const std::string &message) {
                            if (success) {
                                LOG_INFO("{}", message);
                                ProgressOverlay::Complete("client_" + clientName, true, "Removed successfully");
                            } else {
                                LOG_ERROR("{}", message);
                                ProgressOverlay::Complete("client_" + clientName, false, message);
                            }

                            MultiInstance::getAvailableClientsForUI(true);
                        }
                    );
                }
            } else {
                if (ImGui::Button("Install", ImVec2(-FLT_MIN, 0))) {
                    const std::string taskId = "client_" + clientName;

                    ProgressOverlay::Add(taskId, std::format("Installing {}...", clientName), true, [clientName]() {
                        LOG_INFO("Installation cancelled by user: {}", clientName);
                    });

                    auto progressCallback = [taskId](float progress, const std::string &message) {
                        ProgressOverlay::Update(taskId, progress, message);
                    };

                    auto completionCallback = [taskId](bool success, const std::string &message) {
                        if (success) {
                            LOG_INFO("{}", message);
                            ProgressOverlay::Complete(taskId, true, "Installation complete!");
                        } else {
                            LOG_ERROR("{}", message);
                            ProgressOverlay::Complete(taskId, false, message);
                        }

                        MultiInstance::getAvailableClientsForUI(true);
                    };

                    ClientManager::InstallClientAsync(clientName, progressCallback, completionCallback);
                }

                if (disableInstall && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("Please enter a key before installing");
                }
            }

            if (disableButton) {
                ImGui::EndDisabled();
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::Spacing();

    RenderEnvironmentCleanupSection();

#endif // __APPLE__

    ImGui::EndChild();

    if (g_requestOpenConsoleModal) {
        ImGui::OpenPopup("ConsolePopup");
        g_requestOpenConsoleModal = false;
    }

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const ImVec2 desiredSize(vp->WorkSize.x * 0.60f, vp->WorkSize.y * 0.80f);
    ImGui::SetNextWindowSize(desiredSize, ImGuiCond_Always);

    if (ImGui::BeginPopupModal("ConsolePopup", nullptr, ImGuiWindowFlags_NoResize)) {
        const ImGuiStyle &style = ImGui::GetStyle();

        const float closeBtnWidth = ImGui::CalcTextSize("Close").x + style.FramePadding.x * 2.0f;
        const float closeBtnHeight = ImGui::GetFrameHeight();

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float childHeight = std::max(0.0f, avail.y - closeBtnHeight - style.ItemSpacing.y);

        ImGui::BeginChild("ConsoleArea", ImVec2(0, childHeight), ImGuiChildFlags_Borders);
        Console::RenderConsoleTab();
        ImGui::EndChild();

        ImGui::Spacing();

        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - closeBtnWidth);
        if (ImGui::Button("Close", ImVec2(closeBtnWidth, 0))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}
