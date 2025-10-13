#include "accounts.h"
#include "accounts_context_menu.h"
#include "accounts_join_ui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <ctime>
#include <imgui.h>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <windows.h> // Explicitly include windows.h first

#include "../webview_helpers.h"
#include "main_thread.h"
#include "webview.hpp"

#include "core/logging.hpp"
#include "core/status.h"
#include "core/time_utils.h"
#include "network/roblox.h"
#include "system/threading.h"

#include "../../ui.h"
#include "../components.h"
#include "../data.h"

using namespace ImGui;
using namespace std;

static bool s_openUrlPopup = false;
static int s_urlPopupAccountId = -1;
static char s_urlBuffer[256] = "";
static std::unordered_set<int> s_voiceUpdateInProgress;

void RenderAccountsTable(vector<AccountData> &accounts_to_display, const char *table_id, float table_height) {
	constexpr int column_count = 6;
	ImGuiTableFlags table_flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable
								| ImGuiTableFlags_ScrollY | ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable
								| ImGuiTableFlags_ContextMenuInBody;

	if (g_selectedAccountIds.empty() && g_defaultAccountId != -1) { g_selectedAccountIds.insert(g_defaultAccountId); }

	if (BeginTable(table_id, column_count, table_flags, ImVec2(0.0f, table_height > 0 ? table_height - 2.0f : 0.0f))) {
		TableSetupColumn("Display Name", ImGuiTableColumnFlags_WidthStretch, 1.0000f);
		TableSetupColumn("Username", ImGuiTableColumnFlags_WidthStretch, 1.0000f);
		TableSetupColumn("UserID", ImGuiTableColumnFlags_WidthStretch, 0.7000f);
		TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 0.5000f);
		TableSetupColumn("Voice", ImGuiTableColumnFlags_WidthStretch, 0.4500f);
		TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch, 2.0000f);
		TableSetupScrollFreeze(0, 1);

		TableNextRow(ImGuiTableRowFlags_Headers);
		TableNextColumn();
		TextUnformatted("Display Name");
		TableNextColumn();
		TextUnformatted("Username");
		TableNextColumn();
		TextUnformatted("UserID");
		TableNextColumn();
		TextUnformatted("Status");
		TableNextColumn();
		TextUnformatted("Voice");
		TableNextColumn();
		TextUnformatted("Note");

		for (auto &account : accounts_to_display) {
			TableNextRow();
			PushID(account.id);

			bool is_row_selected = g_selectedAccountIds.contains(account.id);

			if (is_row_selected) { TableSetBgColor(ImGuiTableBgTarget_RowBg0, GetColorU32(ImGuiCol_Header)); }

			float row_interaction_height = GetFrameHeight();
			if (row_interaction_height <= 0) { row_interaction_height = GetTextLineHeightWithSpacing(); }
			if (row_interaction_height <= 0) { row_interaction_height = 19.0f; }

			float text_visual_height = GetTextLineHeight();
			float vertical_padding = (row_interaction_height - text_visual_height) * 0.5f;
			vertical_padding = ImMax(0.0f, vertical_padding);

			TableNextColumn();
			float cell_content_start_y = GetCursorPosY();

			char selectable_label[64];
			snprintf(selectable_label, sizeof(selectable_label), "##row_selectable_%d", account.id);

			bool banned = account.status == "Banned" || account.status == "Warned" || account.status == "Terminated";
			if (Selectable(
					selectable_label,
					is_row_selected,
					ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap,
					ImVec2(0, row_interaction_height)
				)) {
				if (GetIO().KeyCtrl) {
					if (is_row_selected) {
						g_selectedAccountIds.erase(account.id);
					} else {
						g_selectedAccountIds.insert(account.id);
					}
				} else {
					bool was_already_solely_selected = (is_row_selected && g_selectedAccountIds.size() == 1);
					g_selectedAccountIds.clear();
					if (!was_already_solely_selected) { g_selectedAccountIds.insert(account.id); }
				}
			}

			static std::unordered_map<int, double> holdStartTimes;
			if (IsItemActivated() && IsMouseDown(ImGuiMouseButton_Left)) { holdStartTimes[account.id] = GetTime(); }

			bool holdTriggered = false;
			if (IsItemActive()) {
				auto it = holdStartTimes.find(account.id);
				if (it != holdStartTimes.end() && (GetTime() - it->second) >= 0.65f) {
					holdStartTimes.erase(it);
					holdTriggered = true;
				}
			} else {
				holdStartTimes.erase(account.id);
			}

			if (IsItemHovered() && IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				if (!account.cookie.empty()) {
					LOG_INFO(
						"Opening browser for account: " + account.displayName + " (ID: " + std::to_string(account.id)
						+ ")"
					);
					Threading::newThread([acc = account]() { LaunchBrowserWithCookie(acc); });
				} else {
					LOG_WARN("Cannot open browser - cookie is empty for account: " + account.displayName);
					Status::Error("Cookie is empty for this account");
				}
			}

			if (holdTriggered) {
				if (!account.cookie.empty()) {
					s_openUrlPopup = true;
					s_urlPopupAccountId = account.id;
					s_urlBuffer[0] = '\0';
				} else {
					LOG_WARN("Cannot open browser - cookie is empty for account: " + account.displayName);
					Status::Error("Cookie is empty for this account");
				}
			}

			string context_menu_id = string(table_id) + "_ContextMenu_" + to_string(account.id);

			RenderAccountContextMenu(account, context_menu_id);

			SetItemAllowOverlap();

			SetCursorPosY(cell_content_start_y + vertical_padding);
			TextUnformatted(account.displayName.c_str());

			SetCursorPosY(cell_content_start_y + row_interaction_height);

			auto render_centered_text_in_cell = [&](const char *text, ImVec4 *color = nullptr) {
				TableNextColumn();
				float current_cell_start_y = GetCursorPosY();

				SetCursorPosY(current_cell_start_y + vertical_padding);
				if (color) {
					TextColored(*color, "%s", text);
				} else {
					TextUnformatted(text);
				}

				SetCursorPosY(current_cell_start_y + row_interaction_height);
			};

			render_centered_text_in_cell(account.username.c_str());
			render_centered_text_in_cell(account.userId.c_str());

			ImVec4 statusColor = getStatusColor(account.status);
			TableNextColumn();
			float status_y = GetCursorPosY();
			SetCursorPosY(status_y + vertical_padding);
			TextColored(statusColor, "%s", account.status.c_str());
			if (IsItemHovered()) {
				if (account.status == "Banned" && account.banExpiry > 0) {
					BeginTooltip();
					string timeStr = formatCountdown(account.banExpiry);
					TextUnformatted(timeStr.c_str());
					EndTooltip();
				} else if (account.status == "InGame" && !account.lastLocation.empty()) {
					BeginTooltip();
					TextUnformatted(account.lastLocation.c_str());
					EndTooltip();
				}
			}
			SetCursorPosY(status_y + row_interaction_height);

			TableNextColumn();
			float voice_y = GetCursorPosY();
			SetCursorPosY(voice_y + vertical_padding);
			ImVec4 voiceCol = ImVec4(1.f, 1.f, 1.f, 1.f);
			if (account.voiceStatus == "Enabled") {
				voiceCol = ImVec4(0.7f, 1.f, 0.7f, 1.f); // Pastel green
			} else if (account.voiceStatus == "Disabled") {
				voiceCol = ImVec4(1.f, 1.f, 0.7f, 1.f); // Pastel yellow
			} else if (account.voiceStatus == "Banned") {
				voiceCol = ImVec4(1.f, 0.7f, 0.7f, 1.f); // Pastel red
			} else if (account.voiceStatus == "N/A") {
				voiceCol = ImVec4(0.7f, 0.7f, 0.7f, 1.f); // Darker gray for N/A
			}

			if (account.voiceStatus == "Banned" && account.voiceBanExpiry > 0) {
				time_t now = time(nullptr);
				if (now >= account.voiceBanExpiry && !account.cookie.empty()
					&& s_voiceUpdateInProgress.count(account.id) == 0) {
					s_voiceUpdateInProgress.insert(account.id);
					int accId = account.id;
					string cookie = account.cookie;
					Threading::newThread([accId, cookie]() {
						auto vs = Roblox::getVoiceChatStatus(cookie);
						MainThread::Post([accId, vs]() {
							auto it = find_if(g_accounts.begin(), g_accounts.end(), [&](const AccountData &a) {
								return a.id == accId;
							});
							if (it != g_accounts.end()) {
								it->voiceStatus = vs.status;
								it->voiceBanExpiry = vs.bannedUntil;
							}
							s_voiceUpdateInProgress.erase(accId);
							Data::SaveAccounts();
						});
					});
				}
			}

			TextColored(voiceCol, "%s", account.voiceStatus.c_str());
			if (IsItemHovered()) {
				if (account.voiceStatus == "Banned" && account.voiceBanExpiry > 0) {
					BeginTooltip();
					string timeStr = formatCountdown(account.voiceBanExpiry);
					TextUnformatted(timeStr.c_str());
					EndTooltip();
				} else if (account.voiceStatus == "Unknown") {
					BeginTooltip();
					TextUnformatted("HTTP request returned an error");
					EndTooltip();
				} else if (account.voiceStatus == "N/A") {
					BeginTooltip();
					TextUnformatted("HTTP request unavailable");
					EndTooltip();
				}
			}
			SetCursorPosY(voice_y + row_interaction_height);

			render_centered_text_in_cell(account.note.c_str());

			PopID();
		}
		EndTable();
	}

	if (s_openUrlPopup) {
		OpenPopup("Open URL");
		s_openUrlPopup = false;
	}
	if (BeginPopupModal("Open URL", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGuiStyle &style = GetStyle();
		float openWidth = CalcTextSize("Open").x + style.FramePadding.x * 2.0f;
		float cancelWidth = CalcTextSize("Cancel").x + style.FramePadding.x * 2.0f;
		float inputWidth = GetContentRegionAvail().x - openWidth - cancelWidth - style.ItemSpacing.x;
		float minField = GetFontSize() * 6.25f; // ~100px at 16px
		if (inputWidth < minField) { inputWidth = minField; }
		PushItemWidth(inputWidth);
		InputTextWithHint("##WebviewUrl", "Enter URL", s_urlBuffer, sizeof(s_urlBuffer));
		PopItemWidth();
		Spacing();
		if (Button("Open", ImVec2(openWidth, 0)) && s_urlBuffer[0] != '\0') {
			auto it = find_if(g_accounts.begin(), g_accounts.end(), [&](const AccountData &a) {
				return a.id == s_urlPopupAccountId;
			});
			if (it != g_accounts.end()) {
				string url = s_urlBuffer;
				Threading::newThread([acc = *it, url]() { LaunchWebview(url, acc); });
			}
			s_urlBuffer[0] = '\0';
			CloseCurrentPopup();
		}
		SameLine(0, style.ItemSpacing.x);
		if (Button("Cancel", ImVec2(cancelWidth, 0))) {
			s_urlBuffer[0] = '\0';
			CloseCurrentPopup();
		}
		EndPopup();
	}
}

void RenderFullAccountsTabContent() {
	float availH = GetContentRegionAvail().y;
	ImGuiStyle &style = GetStyle();

	float join_options_section_height = 0;

	join_options_section_height += GetTextLineHeight() + style.ItemSpacing.y;
	join_options_section_height += GetFrameHeight() + style.ItemSpacing.y;
	if (join_type_combo_index == 1) {
		join_options_section_height += GetFrameHeight() + style.ItemSpacing.y;
		join_options_section_height += GetFrameHeight() + style.ItemSpacing.y;
	} else {
		join_options_section_height += GetFrameHeight() + style.ItemSpacing.y;
	}
	join_options_section_height += 1.0f + style.ItemSpacing.y;
	join_options_section_height += GetFrameHeight() + style.ItemSpacing.y;
	join_options_section_height += style.ItemSpacing.y;

	float separator_height_after_table = 1.0f + style.ItemSpacing.y;

	float total_height_for_join_ui_and_sep = separator_height_after_table + join_options_section_height;

	float tableH = ImMax(GetFrameHeight() * 3.0f, availH - total_height_for_join_ui_and_sep);
	if (tableH < GetFrameHeight() * 3.0f) { tableH = GetFrameHeight() * 3.0f; }
	if (availH <= total_height_for_join_ui_and_sep) { tableH = GetFrameHeight() * 3.0f; }

	RenderAccountsTable(g_accounts, "AccountsTable", tableH);

	Separator();
	RenderJoinOptions();
}
