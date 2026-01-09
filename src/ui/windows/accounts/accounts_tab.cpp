#include "accounts_context_menu.h"
#include "accounts_context_menu.h"
#include "accounts_join_ui.h"
#include "imgui_internal.h"
#include "accounts.h"
#include <imgui.h>
#include <random>
#include <string>
#include <algorithm>
#include <vector>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <ctime>
#include <ranges>
#include <format>

#include "main_thread.h"
#include "ui/webview/webview.h"

#include "console/console.h"
#include "ui/widgets/bottom_right_status.h"
#include "utils/time_utils.h"
#include "network/roblox/common.h"
#include "network/roblox/auth.h"
#include "network/roblox/games.h"
#include "network/roblox/session.h"
#include "network/roblox/social.h"
#include "utils/threading.h"

#include "../components.h"
#include "../../ui.h"
#include "components/data.h"

namespace {
    constexpr int COLUMN_COUNT = 6;
    constexpr float HOLD_THRESHOLD_SECONDS = 0.65f;
    constexpr float DEFAULT_ROW_HEIGHT = 19.0f;
    constexpr float MIN_INPUT_WIDTH = 100.0f;

    constexpr float COL_DISPLAY_NAME_WEIGHT = 1.0000f;
    constexpr float COL_USERNAME_WEIGHT = 1.0000f;
    constexpr float COL_USERID_WEIGHT = 0.7000f;
    constexpr float COL_STATUS_WEIGHT = 0.5000f;
    constexpr float COL_VOICE_WEIGHT = 0.4500f;
    constexpr float COL_NOTE_WEIGHT = 2.0000f;

    constexpr ImVec4 COLOR_VOICE_ENABLED{0.7f, 1.0f, 0.7f, 1.0f};   // Pastel green
    constexpr ImVec4 COLOR_VOICE_DISABLED{1.0f, 1.0f, 0.7f, 1.0f};  // Pastel yellow
    constexpr ImVec4 COLOR_VOICE_BANNED{1.0f, 0.7f, 0.7f, 1.0f};    // Pastel red
    constexpr ImVec4 COLOR_VOICE_NA{0.7f, 0.7f, 0.7f, 1.0f};        // Gray

	struct DragDropState {
		int draggedIndex = -1;
		bool isDragging = false;
		ImVec4 dragIndicatorColor = ImVec4(0.4f, 0.6f, 1.0f, 0.8f);
	};

	DragDropState g_dragDropState;

    struct UrlPopupState {
        bool open{false};
        int accountId{-1};
        char buffer[256]{};
    };

    UrlPopupState g_urlPopup;
    std::unordered_set<int> g_voiceUpdateInProgress;
    std::unordered_map<int, double> g_holdStartTimes;

    ImVec4 getVoiceStatusColor(std::string_view status) noexcept {
        if (status == "Enabled") return COLOR_VOICE_ENABLED;
        if (status == "Disabled") return COLOR_VOICE_DISABLED;
        if (status == "Banned") return COLOR_VOICE_BANNED;
        if (status == "N/A") return COLOR_VOICE_NA;
        return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    void handleAccountSelection(int accountId, bool isCurrentlySelected) {
        if (ImGui::GetIO().KeyCtrl) {
            if (isCurrentlySelected) {
                g_selectedAccountIds.erase(accountId);
            } else {
                g_selectedAccountIds.insert(accountId);
            }
        } else {
            const bool wasSolelySelected = isCurrentlySelected && g_selectedAccountIds.size() == 1;
            g_selectedAccountIds.clear();
            if (!wasSolelySelected) {
                g_selectedAccountIds.insert(accountId);
            }
        }
    }

    void handleDoubleClick(const AccountData& account) {
        if (account.cookie.empty()) {
            LOG_WARN("Cannot open browser - cookie is empty for account: {}", account.displayName);
            Status::Error("Cookie is empty for this account");
            return;
        }

        LOG_INFO("Opening browser for account: {} (ID: {})",
                            account.displayName, account.id);
        Threading::newThread([account]() { 
            LaunchBrowserWithCookie(account); 
        });
    }

    void handleHoldAction(const AccountData& account) {
        if (account.cookie.empty()) {
            LOG_WARN("Cannot open browser - cookie is empty for account: {}", account.displayName);
            Status::Error("Cookie is empty for this account");
            return;
        }

        g_urlPopup.open = true;
        g_urlPopup.accountId = account.id;
        g_urlPopup.buffer[0] = '\0';
    }

    void checkVoiceBanExpiry(AccountData& account) {
        if (account.voiceStatus != "Banned" || account.voiceBanExpiry <= 0) return;
        if (account.cookie.empty()) return;
        if (g_voiceUpdateInProgress.contains(account.id)) return;

        const auto now = std::time(nullptr);
        if (now < account.voiceBanExpiry) return;

        g_voiceUpdateInProgress.insert(account.id);
        const int accountId = account.id;
        const std::string cookie = account.cookie;

        Threading::newThread([accountId, cookie]() {
            const auto voiceStatus = Roblox::getVoiceChatStatus(cookie);
            MainThread::Post([accountId, voiceStatus]() {
                const auto it = std::ranges::find_if(g_accounts, 
                    [accountId](const auto& a) { return a.id == accountId; });
                
                if (it != g_accounts.end()) {
                    it->voiceStatus = voiceStatus.status;
                    it->voiceBanExpiry = voiceStatus.bannedUntil;
                }
                g_voiceUpdateInProgress.erase(accountId);
                Data::SaveAccounts();
            });
        });
    }

    struct RowMetrics {
        float height;
        float verticalPadding;
    };

    RowMetrics calculateRowMetrics() {
        float height = ImGui::GetFrameHeight();
        if (height <= 0.0f) height = ImGui::GetTextLineHeightWithSpacing();
        if (height <= 0.0f) height = DEFAULT_ROW_HEIGHT;

        const float textHeight = ImGui::GetTextLineHeight();
        const float padding = std::max(0.0f, (height - textHeight) * 0.5f);

        return {height, padding};
    }

    void renderCenteredTextCell(std::string_view text, float cellStartY, 
                                float rowHeight, float verticalPadding, 
                                const ImVec4* color = nullptr) {
        ImGui::TableNextColumn();
        const float currentY = ImGui::GetCursorPosY();

        ImGui::SetCursorPosY(currentY + verticalPadding);
        if (color) {
            ImGui::TextColored(*color, "%s", text.data());
        } else {
            ImGui::TextUnformatted(text.data());
        }

        ImGui::SetCursorPosY(currentY + rowHeight);
    }

    void renderStatusCell(const AccountData& account, float startY, 
                         float rowHeight, float verticalPadding) {
        ImGui::TableNextColumn();
        const float currentY = ImGui::GetCursorPosY();

        ImGui::SetCursorPosY(currentY + verticalPadding);
        const ImVec4 statusColor = getStatusColor(account.status);
        ImGui::TextColored(statusColor, "%s", account.status.c_str());

        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            if (account.status == "Banned" && account.banExpiry > 0) {
                const auto timeStr = formatCountdown(account.banExpiry);
                ImGui::TextUnformatted(timeStr.c_str());
            } else if (account.status == "InGame" && !account.lastLocation.empty()) {
                ImGui::TextUnformatted(account.lastLocation.c_str());
            }
            ImGui::EndTooltip();
        }

        ImGui::SetCursorPosY(currentY + rowHeight);
    }

    void renderVoiceCell(AccountData& account, float startY, 
                        float rowHeight, float verticalPadding) {
        ImGui::TableNextColumn();
        const float currentY = ImGui::GetCursorPosY();

        checkVoiceBanExpiry(account);

        ImGui::SetCursorPosY(currentY + verticalPadding);
        const ImVec4 voiceColor = getVoiceStatusColor(account.voiceStatus);
        ImGui::TextColored(voiceColor, "%s", account.voiceStatus.c_str());

        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            if (account.voiceStatus == "Banned" && account.voiceBanExpiry > 0) {
                const auto timeStr = formatCountdown(account.voiceBanExpiry);
                ImGui::TextUnformatted(timeStr.c_str());
            } else if (account.voiceStatus == "Unknown") {
                ImGui::TextUnformatted("HTTP request returned an error");
            } else if (account.voiceStatus == "N/A") {
                ImGui::TextUnformatted("HTTP request unavailable");
            }
            ImGui::EndTooltip();
        }

        ImGui::SetCursorPosY(currentY + rowHeight);
    }

	void renderDragDropTarget(int targetIndex, const std::vector<AccountData>& accounts) {
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ACCOUNT_ROW_REORDER")) {
                const int sourceIndex = *(const int*)payload->Data;

                if (sourceIndex != targetIndex &&
                    sourceIndex >= 0 && sourceIndex < accounts.size() &&
                    targetIndex >= 0 && targetIndex < accounts.size()) {

                    // Perform the reorder
                    auto accountCopy = g_accounts[sourceIndex];
                    g_accounts.erase(g_accounts.begin() + sourceIndex);

                    // Adjust target index if source was before target
                    int insertIndex = targetIndex;
                    if (sourceIndex < targetIndex) {
                        insertIndex--;
                    }

                    g_accounts.insert(g_accounts.begin() + insertIndex, accountCopy);
                    Data::SaveAccounts();

                    LOG_INFO("Reordered account from index {} to {}",
                                        sourceIndex, insertIndex);
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    void renderDragDropIndicator(int currentIndex, int draggedIndex) {
        if (!g_dragDropState.isDragging || draggedIndex < 0) return;

        // Draw a line indicator where the item will be dropped
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();

            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImU32 color = ImGui::GetColorU32(g_dragDropState.dragIndicatorColor);

            // Draw horizontal line at the top or bottom depending on relative position
            const float lineThickness = 3.0f;
            const float y = (currentIndex < draggedIndex) ? min.y : max.y;

            drawList->AddLine(
                ImVec2(min.x, y),
                ImVec2(max.x, y),
                color,
                lineThickness
            );
        }
    }

    void renderAccountRow(AccountData& account, const RowMetrics& metrics) {
        ImGui::TableNextRow();
        ImGui::PushID(account.id);

        const auto it = std::ranges::find_if(g_accounts,
            [&account](const auto& a) { return a.id == account.id; });
        const int currentIndex = (it != g_accounts.end()) ?
            std::distance(g_accounts.begin(), it) : -1;

        const bool isSelected = g_selectedAccountIds.contains(account.id);
        if (isSelected) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                  ImGui::GetColorU32(ImGuiCol_Header));
        }

        if (g_dragDropState.isDragging && currentIndex == g_dragDropState.draggedIndex) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                ImGui::GetColorU32(ImVec4(0.3f, 0.5f, 0.8f, 0.3f)));
        }

        ImGui::TableNextColumn();
        const float cellStartY = ImGui::GetCursorPosY();

        const auto selectableLabel = std::format("##row_selectable_{}", account.id);
        if (ImGui::Selectable(selectableLabel.c_str(), isSelected,
                             ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                             ImVec2(0, metrics.height))) {
            handleAccountSelection(account.id, isSelected);
        }

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
            ImGui::SetDragDropPayload("ACCOUNT_ROW_REORDER", &currentIndex, sizeof(int));
            g_dragDropState.isDragging = true;
            g_dragDropState.draggedIndex = currentIndex;

            // Show preview
            ImGui::Text("Moving: %s", account.displayName.c_str());
            ImGui::EndDragDropSource();
        }

        // Reset drag state when not dragging
        if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            g_dragDropState.isDragging = false;
            g_dragDropState.draggedIndex = -1;
        }

        if (currentIndex >= 0) {
            renderDragDropTarget(currentIndex, g_accounts);
            renderDragDropIndicator(currentIndex, g_dragDropState.draggedIndex);
        }

        // Handle hold gesture
        if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g_holdStartTimes[account.id] = ImGui::GetTime();
        }

        bool holdTriggered = false;
        if (ImGui::IsItemActive()) {
            if (const auto it = g_holdStartTimes.find(account.id); 
                it != g_holdStartTimes.end() && 
                (ImGui::GetTime() - it->second) >= HOLD_THRESHOLD_SECONDS) {
                g_holdStartTimes.erase(it);
                holdTriggered = true;
            }
        } else {
            g_holdStartTimes.erase(account.id);
        }

        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            handleDoubleClick(account);
        }

        if (holdTriggered) {
            handleHoldAction(account);
        }

        const auto contextMenuId = std::format("AccountsTable_ContextMenu_{}", account.id);
        RenderAccountContextMenu(account, contextMenuId);

        ImGui::SetNextItemAllowOverlap();
        ImGui::SetCursorPosY(cellStartY + metrics.verticalPadding);
        ImGui::TextUnformatted(account.displayName.c_str());
        ImGui::SetCursorPosY(cellStartY + metrics.height);

        renderCenteredTextCell(account.username, cellStartY, metrics.height, metrics.verticalPadding);
        renderCenteredTextCell(account.userId, cellStartY, metrics.height, metrics.verticalPadding);
        renderStatusCell(account, cellStartY, metrics.height, metrics.verticalPadding);
        renderVoiceCell(account, cellStartY, metrics.height, metrics.verticalPadding);
        renderCenteredTextCell(account.note, cellStartY, metrics.height, metrics.verticalPadding);

        ImGui::PopID();
    }

    void renderUrlPopup() {
        if (g_urlPopup.open) {
            ImGui::OpenPopup("Open URL");
            g_urlPopup.open = false;
        }

        if (!ImGui::BeginPopupModal("Open URL", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            return;
        }

        const auto& style = ImGui::GetStyle();
        const float openWidth = ImGui::CalcTextSize("Open").x + style.FramePadding.x * 2.0f;
        const float cancelWidth = ImGui::CalcTextSize("Cancel").x + style.FramePadding.x * 2.0f;
        const float inputWidth = std::max(MIN_INPUT_WIDTH, 
            ImGui::GetContentRegionAvail().x - openWidth - cancelWidth - style.ItemSpacing.x);

        ImGui::PushItemWidth(inputWidth);
        ImGui::InputTextWithHint("##WebviewUrl", "Enter URL", g_urlPopup.buffer, sizeof(g_urlPopup.buffer));
        ImGui::PopItemWidth();
        ImGui::Spacing();

        if (ImGui::Button("Open", ImVec2(openWidth, 0)) && g_urlPopup.buffer[0] != '\0') {
            const auto it = std::ranges::find_if(g_accounts, 
                [](const auto& a) { return a.id == g_urlPopup.accountId; });
            
            if (it != g_accounts.end()) {
                Threading::newThread([account = *it, url = std::string(g_urlPopup.buffer)]() {
                    LaunchWebview(url, account);
                });
            }
            g_urlPopup.buffer[0] = '\0';
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine(0, style.ItemSpacing.x);
        if (ImGui::Button("Cancel", ImVec2(cancelWidth, 0))) {
            g_urlPopup.buffer[0] = '\0';
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    float calculateJoinOptionsHeight(JoinType joinType) {
        const auto& style = ImGui::GetStyle();
        float height = 0.0f;

        // Title + help marker
        height += ImGui::GetTextLineHeight() + style.ItemSpacing.y;
        
        // Combo box
        height += ImGui::GetFrameHeight() + style.ItemSpacing.y;
        
        // Input fields
        if (joinType == JoinType::GameServer) {
            height += ImGui::GetFrameHeight() + style.ItemSpacing.y;
            height += ImGui::GetFrameHeight() + style.ItemSpacing.y;
        } else {
            height += ImGui::GetFrameHeight() + style.ItemSpacing.y;
        }
        
        // Separator
        height += 1.0f + style.ItemSpacing.y;

        // Buttons
        height += ImGui::GetFrameHeight() + style.ItemSpacing.y;
        height += style.ItemSpacing.y;

        return height;
    }

}

void RenderAccountsTable(std::vector<AccountData>& accountsToDisplay, 
                        const char* tableId, float tableHeight) {
    // Auto-select default account if nothing selected
    if (g_selectedAccountIds.empty() && g_defaultAccountId != -1) {
        g_selectedAccountIds.insert(g_defaultAccountId);
    }

    constexpr ImGuiTableFlags tableFlags = 
        ImGuiTableFlags_Borders | 
        ImGuiTableFlags_RowBg | 
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY | 
        ImGuiTableFlags_Hideable | 
        ImGuiTableFlags_Reorderable |
        ImGuiTableFlags_ContextMenuInBody;

    const ImVec2 tableSize(0.0f, tableHeight > 0.0f ? tableHeight - 2.0f : 0.0f);
    if (!ImGui::BeginTable(tableId, COLUMN_COUNT, tableFlags, tableSize)) {
        return;
    }

    ImGui::TableSetupColumn("Display Name", ImGuiTableColumnFlags_WidthStretch, COL_DISPLAY_NAME_WEIGHT);
    ImGui::TableSetupColumn("Username", ImGuiTableColumnFlags_WidthStretch, COL_USERNAME_WEIGHT);
    ImGui::TableSetupColumn("UserID", ImGuiTableColumnFlags_WidthStretch, COL_USERID_WEIGHT);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, COL_STATUS_WEIGHT);
    ImGui::TableSetupColumn("Voice", ImGuiTableColumnFlags_WidthStretch, COL_VOICE_WEIGHT);
    ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch, COL_NOTE_WEIGHT);
    ImGui::TableSetupScrollFreeze(0, 1);

    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    ImGui::TableNextColumn(); ImGui::TextUnformatted("Display Name");
    ImGui::TableNextColumn(); ImGui::TextUnformatted("Username");
    ImGui::TableNextColumn(); ImGui::TextUnformatted("UserID");
    ImGui::TableNextColumn(); ImGui::TextUnformatted("Status");
    ImGui::TableNextColumn(); ImGui::TextUnformatted("Voice");
    ImGui::TableNextColumn(); ImGui::TextUnformatted("Note");

    const auto metrics = calculateRowMetrics();
    for (auto& account : accountsToDisplay) {
        renderAccountRow(account, metrics);
    }

    ImGui::EndTable();
}

void RenderFullAccountsTabContent() {
    const float availHeight = ImGui::GetContentRegionAvail().y;
    const auto& style = ImGui::GetStyle();

    const float joinOptionsHeight = calculateJoinOptionsHeight(static_cast<JoinType>(join_type_combo_index));
    const float separatorHeight = 1.0f + style.ItemSpacing.y;
    const float totalReserved = separatorHeight + joinOptionsHeight;

    // Calculate table height with minimum constraints
    constexpr float MIN_TABLE_HEIGHT_MULTIPLIER = 3.0f;
    const float minTableHeight = ImGui::GetFrameHeight() * MIN_TABLE_HEIGHT_MULTIPLIER;
    
    float tableHeight = std::max(minTableHeight, availHeight - totalReserved);
    if (availHeight <= totalReserved) {
        tableHeight = minTableHeight;
    }

    RenderAccountsTable(g_accounts, "AccountsTable", tableHeight);
    ImGui::Separator();
    RenderJoinOptions();
}