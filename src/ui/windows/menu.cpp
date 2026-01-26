#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <future>
#include <print>
#include <string>
#include <vector>

#include <imgui.h>

#include "components.h"
#include "console/console.h"
#include "data.h"
#include "network/roblox/common.h"
#include "network/roblox/auth.h"
#include "network/roblox/common.h"
#include "network/roblox/games.h"
#include "network/roblox/session.h"
#include "network/roblox/social.h"
#include "system/multi_instance.h"
#include "system/roblox_control.h"
#include "ui/webview/webview.h"
#include "ui/widgets/bottom_right_status.h"
#include "ui/widgets/modal_popup.h"
#include "ui/windows/backup/backup.h"
#include "utils/paths.h"
#include "utils/thread_task.h"

struct DuplicateAccountModalState {
        bool showModal = false;
        std::string pendingCookie;
        std::string pendingUsername;
        std::string pendingDisplayName;
        std::string pendingPresence;
        std::string pendingUserId;
        Roblox::VoiceSettings pendingVoiceStatus;
        int existingId = -1;
        int nextId = -1;
};

static DuplicateAccountModalState g_duplicateAccountModal;

namespace {
    constexpr size_t COOKIE_BUFFER_SIZE = 2048;
    constexpr size_t PASSWORD_BUFFER_SIZE = 128;

    std::string TrimWhitespace(const std::string &str) {
        const auto start = str.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) {
            return "";
        }
        const auto end = str.find_last_not_of(" \t\r\n");
        return str.substr(start, end - start + 1);
    }

    int GetMaxAccountId() {
        int maxId = 0;
        for (const auto &acct: g_accounts) {
            if (acct.id > maxId) {
                maxId = acct.id;
            }
        }
        return maxId;
    }

    void RefreshAccountStatuses() {
        ThreadTask::fireAndForget([] {
            LOG_INFO("Refreshing account statuses...");

            std::vector<std::future<std::pair<int, std::optional<Roblox::FullAccountInfo>>>> futures;
            futures.reserve(g_accounts.size());

            for (const auto &acct: g_accounts) {
                futures.push_back(std::async(std::launch::async, [cookie = acct.cookie, id = acct.id]() {
                    auto info = Roblox::fetchFullAccountInfo(cookie);
                    if (info) {
                        return std::make_pair(id, std::make_optional(*info));
                    }
                    return std::make_pair(id, std::optional<Roblox::FullAccountInfo> {});
                }));
            }

            for (auto &fut: futures) {
                auto [accountId, infoOpt] = fut.get();

                auto it = std::ranges::find_if(g_accounts, [accountId](const AccountData &a) {
                    return a.id == accountId;
                });

                if (it == g_accounts.end()) {
                    continue;
                }

                if (!infoOpt) {
                    // Keep existing data on network error
                    continue;
                }

                const auto &info = *infoOpt;

                switch (info.banInfo.status) {
                    case Roblox::BanCheckResult::Banned:
                        it->status = "Banned";
                        it->banExpiry = info.banInfo.endDate;
                        it->voiceStatus = "N/A";
                        it->voiceBanExpiry = 0;
                        continue;

                    case Roblox::BanCheckResult::Warned:
                        it->status = "Warned";
                        it->voiceStatus = "N/A";
                        it->voiceBanExpiry = 0;
                        continue;

                    case Roblox::BanCheckResult::Terminated:
                        it->status = "Terminated";
                        it->voiceStatus = "N/A";
                        it->voiceBanExpiry = 0;
                        continue;

                    default:
                        break;
                }

                // Update presence - fetch full presence data for location info
                if (!it->userId.empty()) {
                    const uint64_t uid = std::stoull(it->userId);
                    auto presenceData = Roblox::getPresenceData(it->cookie, uid);
                    if (presenceData) {
                        it->status = presenceData->presence;
                        it->lastLocation = presenceData->lastLocation;
                        it->placeId = presenceData->placeId;
                        it->jobId = presenceData->jobId;
                    } else {
                        it->status = info.presence;
                        it->lastLocation.clear();
                        it->placeId = 0;
                        it->jobId.clear();
                    }
                }

                it->voiceStatus = info.voiceSettings.status;
                it->voiceBanExpiry = info.voiceSettings.bannedUntil;
            }

            Data::SaveAccounts();
            LOG_INFO("Refreshed account statuses");
        });
    }

    void ShowDuplicateAccountPrompt(
        const std::string &cookie,
        const std::string &username,
        const std::string &displayName,
        const std::string &presence,
        const std::string &userId,
        const Roblox::VoiceSettings &voiceSettings,
        int existingId,
        int nextId
    ) {
        g_duplicateAccountModal.pendingCookie = cookie;
        g_duplicateAccountModal.pendingUsername = username;
        g_duplicateAccountModal.pendingDisplayName = displayName;
        g_duplicateAccountModal.pendingPresence = presence;
        g_duplicateAccountModal.pendingUserId = userId;
        g_duplicateAccountModal.pendingVoiceStatus = voiceSettings;
        g_duplicateAccountModal.existingId = existingId;
        g_duplicateAccountModal.nextId = nextId;
        g_duplicateAccountModal.showModal = true;
    }

    void CreateNewAccount(
        int id,
        const std::string &cookie,
        const std::string &userId,
        const std::string &username,
        const std::string &displayName,
        const std::string &presence,
        const Roblox::VoiceSettings &voiceSettings
    ) {
        AccountData newAcct;
        newAcct.id = id;
        newAcct.cookie = cookie;
        newAcct.userId = userId;
        newAcct.username = username;
        newAcct.displayName = displayName;
        newAcct.status = presence;
        newAcct.voiceStatus = voiceSettings.status;
        newAcct.voiceBanExpiry = voiceSettings.bannedUntil;
        newAcct.note = "";
        newAcct.isFavorite = false;

        g_accounts.push_back(std::move(newAcct));
        invalidateAccountIndex();

        LOG_INFO("Added new account {} - {}", id, displayName);
        Data::SaveAccounts();
    }

    bool ValidateAndAddCookie(const std::string &cookie) {
        const std::string trimmedCookie = TrimWhitespace(cookie);

        if (trimmedCookie.empty()) {
            BottomRightStatus::Error("Invalid cookie: Cookie cannot be empty");
            return false;
        }

        auto accountInfo = Roblox::fetchFullAccountInfo(trimmedCookie);

        if (!accountInfo) {
            if (accountInfo.error() == Roblox::ApiError::InvalidCookie) {
                BottomRightStatus::Error("Invalid cookie: Unable to authenticate with Roblox");
            } else {
                BottomRightStatus::Error("Invalid cookie: Unable to retrieve user information");
            }
            return false;
        }

        const auto &info = *accountInfo;

        if (info.userId == 0 || info.username.empty() || info.displayName.empty()) {
            BottomRightStatus::Error("Invalid cookie: Unable to retrieve user information");
            return false;
        }

        const int nextId = GetMaxAccountId() + 1;
        const std::string userIdStr = std::to_string(info.userId);

        const auto existingAccount
            = std::find_if(g_accounts.begin(), g_accounts.end(), [&userIdStr](const AccountData &a) {
                  return a.userId == userIdStr;
              });

        if (existingAccount != g_accounts.end()) {
            ShowDuplicateAccountPrompt(
                trimmedCookie,
                info.username,
                info.displayName,
                info.presence,
                userIdStr,
                info.voiceSettings,
                existingAccount->id,
                nextId
            );
        } else {
            CreateNewAccount(
                nextId,
                trimmedCookie,
                userIdStr,
                info.username,
                info.displayName,
                info.presence,
                info.voiceSettings
            );
        }

        return true;
    }
} // namespace

bool RenderMainMenu() {
    static std::array<char, COOKIE_BUFFER_SIZE> s_cookieInputBuffer = {};
    static bool s_openClearCachePopup = false;
    static bool s_openExportPopup = false;
    static bool s_openImportPopup = false;
    static char s_password1[PASSWORD_BUFFER_SIZE] = "";
    static char s_password2[PASSWORD_BUFFER_SIZE] = "";
    static char s_importPassword[PASSWORD_BUFFER_SIZE] = "";
    static std::vector<std::string> s_backupFiles;
    static int s_selectedBackup = 0;
    static bool s_refreshBackupList = false;

    if (!ImGui::BeginMainMenuBar()) {
        return false;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Export Backup")) {
            s_openExportPopup = true;
        }

        if (ImGui::MenuItem("Import Backup")) {
            s_openImportPopup = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Accounts")) {
        if (ImGui::MenuItem("Refresh Statuses")) {
            RefreshAccountStatuses();
        }

        ImGui::Separator();

        if (ImGui::BeginMenu("Add Account")) {
            if (ImGui::BeginMenu("Add via Cookie")) {
                ImGui::TextUnformatted("Enter Cookie:");
                ImGui::PushItemWidth(ImGui::GetFontSize() * 25);
                ImGui::InputText(
                    "##CookieInputSubmenu",
                    s_cookieInputBuffer.data(),
                    s_cookieInputBuffer.size(),
                    ImGuiInputTextFlags_AutoSelectAll
                );
                ImGui::PopItemWidth();

                const bool canAdd = (s_cookieInputBuffer[0] != '\0');
                if (canAdd && ImGui::MenuItem("Add Cookie", nullptr, false, canAdd)) {
                    if (ValidateAndAddCookie(s_cookieInputBuffer.data())) {
                        s_cookieInputBuffer.fill('\0');
                    } else {
                        s_cookieInputBuffer.fill('\0');
                    }
                }
                ImGui::EndMenu();
            }

            if (ImGui::MenuItem("Add via Login")) {
                LaunchWebviewForLogin(
                    "https://www.roblox.com/login",
                    "Login to Roblox",
                    [](const std::string &extractedCookie) {
                        if (!extractedCookie.empty()) {
                            ValidateAndAddCookie(extractedCookie);
                        }
                    }
                );
            }

            ImGui::EndMenu();
        }

        if (!g_selectedAccountIds.empty()) {
            ImGui::Separator();
            const std::string deleteText = std::format("Delete {} Selected", g_selectedAccountIds.size());
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
            if (ImGui::MenuItem(deleteText.c_str())) {
                ModalPopup::AddYesNo("Delete selected accounts?", []() {
                    std::erase_if(g_accounts, [](const AccountData &acct) {
                        return g_selectedAccountIds.count(acct.id);
                    });
                    invalidateAccountIndex();
                    g_selectedAccountIds.clear();
                    Data::SaveAccounts();
                    LOG_INFO("Deleted selected accounts.");
                });
            }
            ImGui::PopStyleColor();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Utilities")) {
        if (ImGui::MenuItem("Kill Roblox")) {
            RobloxControl::KillRobloxProcesses();
        }

        if (ImGui::MenuItem("Clear Roblox Cache")) {
            if (RobloxControl::IsRobloxRunning()) {
                s_openClearCachePopup = true;
            } else {
                ThreadTask::fireAndForget(RobloxControl::ClearRobloxCache);
            }
        }

        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();

    if (s_openClearCachePopup) {
        ImGui::OpenPopup("ClearCacheConfirm");
        s_openClearCachePopup = false;
    }

    if (ImGui::BeginPopupModal("ClearCacheConfirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("RobloxPlayerBeta is running. Do you want to kill it before clearing the cache?");
        ImGui::Spacing();

        const float killW = ImGui::CalcTextSize("Kill").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float dontW = ImGui::CalcTextSize("Don't kill").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float cancelW = ImGui::CalcTextSize("Cancel").x + ImGui::GetStyle().FramePadding.x * 2.0f;

        if (ImGui::Button("Kill", ImVec2(killW, 0))) {
            RobloxControl::KillRobloxProcesses();
            ThreadTask::fireAndForget(RobloxControl::ClearRobloxCache);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button("Don't kill", ImVec2(dontW, 0))) {
            ThreadTask::fireAndForget(RobloxControl::ClearRobloxCache);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button("Cancel", ImVec2(cancelW, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (s_openExportPopup) {
        ImGui::OpenPopup("ExportBackup");
        s_openExportPopup = false;
    }

    if (ImGui::BeginPopupModal("ExportBackup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Password", s_password1, IM_ARRAYSIZE(s_password1), ImGuiInputTextFlags_Password);
        ImGui::InputText("Confirm", s_password2, IM_ARRAYSIZE(s_password2), ImGuiInputTextFlags_Password);

        if (ImGui::Button("Export")) {
            if (std::strcmp(s_password1, s_password2) == 0 && s_password1[0] != '\0') {
                auto result = Backup::Export(s_password1);
                if (result) {
                    ModalPopup::AddInfo("Backup saved.");
                } else {
                    ModalPopup::AddInfo(Backup::errorToString(result.error()).data());
                }
                s_password1[0] = s_password2[0] = '\0';
                ImGui::CloseCurrentPopup();
            } else {
                ModalPopup::AddInfo("Passwords do not match.");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (s_openImportPopup) {
        ImGui::OpenPopup("ImportBackup");
        s_openImportPopup = false;
        s_refreshBackupList = true;
    }

    if (ImGui::BeginPopupModal("ImportBackup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (s_refreshBackupList) {
            s_backupFiles.clear();
            const auto dir = AltMan::Paths::Backups();

            for (const auto &entry: std::filesystem::directory_iterator(dir)) {

                if (!entry.is_regular_file()) {
                    continue;
                }

                const auto &path = entry.path();

                if (path.filename() == ".DS_Store") {
                    continue;
                }

                if (path.extension() != ".dat") {
                    continue;
                }

                s_backupFiles.push_back(entry.path().filename().string());
            }
            std::sort(s_backupFiles.begin(), s_backupFiles.end());
            s_selectedBackup = 0;
            s_refreshBackupList = false;
        }

        const bool importInProgress = Backup::IsImportInProgress();

        if (s_backupFiles.empty()) {
            ImGui::TextUnformatted("No backups found.");
        } else {
            const char *current = s_backupFiles[s_selectedBackup].c_str();
            ImGui::BeginDisabled(importInProgress);
            if (ImGui::BeginCombo("File", current)) {
                for (int i = 0; i < static_cast<int>(s_backupFiles.size()); ++i) {
                    const bool selected = (i == s_selectedBackup);
                    ImGui::PushID(i);
                    if (ImGui::Selectable(s_backupFiles[i].c_str(), selected)) {
                        s_selectedBackup = i;
                    }
                    ImGui::PopID();
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
        }

        ImGui::BeginDisabled(importInProgress);
        ImGui::InputText("Password", s_importPassword, IM_ARRAYSIZE(s_importPassword), ImGuiInputTextFlags_Password);
        ImGui::EndDisabled();

        if (importInProgress) {
            ImGui::TextUnformatted("Importing...");
        }

        ImGui::BeginDisabled(importInProgress);
        if (ImGui::Button("Import")) {
            if (!s_backupFiles.empty()) {
                const auto dir = AltMan::Paths::Backups() / s_backupFiles[s_selectedBackup];
                Backup::ImportAsync(dir.string(), s_importPassword);
                s_importPassword[0] = '\0';
                ImGui::CloseCurrentPopup();
            } else {
                ModalPopup::AddInfo("No backup selected.");
            }
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(importInProgress);
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();

        ImGui::EndPopup();
    }

    if (g_duplicateAccountModal.showModal) {
        ImGui::OpenPopup("DuplicateAccountPrompt");
        g_duplicateAccountModal.showModal = false;
    }

    if (ImGui::BeginPopupModal("DuplicateAccountPrompt", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto existingAccount = std::find_if(g_accounts.begin(), g_accounts.end(), [](const AccountData &a) {
            return a.id == g_duplicateAccountModal.existingId;
        });

        if (existingAccount != g_accounts.end()) {
            const std::string message = std::format(
                "The cookie you entered is for an already existing account ({}). What would you like to do?",
                existingAccount->displayName
            );
            ImGui::TextWrapped("%s", message.c_str());
        } else {
            ImGui::TextWrapped("The cookie you entered is for an already existing account. What would you like to do?");
        }

        ImGui::Spacing();

        if (ImGui::Button("Update", ImVec2(100, 0))) {
            if (AccountData *acc = getAccountById(g_duplicateAccountModal.existingId)) {
                acc->cookie = g_duplicateAccountModal.pendingCookie;
                acc->username = g_duplicateAccountModal.pendingUsername;
                acc->displayName = g_duplicateAccountModal.pendingDisplayName;
                acc->status = g_duplicateAccountModal.pendingPresence;
                acc->voiceStatus = g_duplicateAccountModal.pendingVoiceStatus.status;
                acc->voiceBanExpiry = g_duplicateAccountModal.pendingVoiceStatus.bannedUntil;

                LOG_INFO("Updated existing account {} - {}", acc->id, acc->displayName);
                Data::SaveAccounts();
            }
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(100, 0))) {
            LOG_INFO("Discarded new cookie for existing account {}", g_duplicateAccountModal.existingId);
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Force Add", ImVec2(100, 0))) {
            CreateNewAccount(
                g_duplicateAccountModal.nextId,
                g_duplicateAccountModal.pendingCookie,
                g_duplicateAccountModal.pendingUserId,
                g_duplicateAccountModal.pendingUsername,
                g_duplicateAccountModal.pendingDisplayName,
                g_duplicateAccountModal.pendingPresence,
                g_duplicateAccountModal.pendingVoiceStatus
            );

            LOG_INFO(
                "Force added new account {} - {}",
                g_duplicateAccountModal.nextId,
                g_duplicateAccountModal.pendingDisplayName
            );
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    return false;
}
