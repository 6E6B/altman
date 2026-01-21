#include "friends.h"
#include <imgui.h>
#include <algorithm>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <utility>
#include <cctype>
#include <ranges>
#include <string_view>

#include "utils/account_utils.h"
#include "ui/windows/accounts/accounts_join_ui.h"
#include "ui/widgets/context_menus.h"
#include "components/data.h"
#include "friends_actions.h"
#include "console/console.h"
#include "ui/widgets/bottom_right_status.h"
#include "utils/time_utils.h"
#include "network/roblox/common.h"
#include "network/roblox/auth.h"
#include "network/roblox/session.h"
#include "network/roblox/social.h"
#include "system/roblox_launcher.h"
#include "utils/thread_task.h"
#include "ui/widgets/modal_popup.h"
#include "ui/webview/webview.h"

namespace {
    constexpr std::string_view ICON_TOOL = "\xEF\x82\xAD ";
    constexpr std::string_view ICON_PERSON = "\xEF\x80\x87 ";
    constexpr std::string_view ICON_CONTROLLER = "\xEF\x84\x9B ";
    constexpr std::string_view ICON_REFRESH = "\xEF\x8B\xB1 ";
    constexpr std::string_view ICON_OPEN_LINK = "\xEF\x8A\xBB ";
    constexpr std::string_view ICON_INVENTORY = "\xEF\x8A\x90 ";
    constexpr std::string_view ICON_JOIN = "\xEF\x8B\xB6 ";
    constexpr std::string_view ICON_USER_PLUS = "\xEF\x88\xB4 ";

    constexpr int VIEW_MODE_FRIENDS = 0;
    constexpr int VIEW_MODE_REQUESTS = 1;

    struct FriendsState {
        int selectedFriendIdx{-1};
        Roblox::FriendDetail selectedFriend{};
        std::atomic<bool> friendDetailsLoading{false};
        std::atomic<bool> friendsLoading{false};
        std::vector<FriendInfo> unfriended;
        int lastAccountId{-1};
        int viewAccountId{-1};
        int viewMode{VIEW_MODE_FRIENDS};
        int lastViewMode{-1};
    };

    struct RequestsState {
        std::vector<Roblox::IncomingFriendRequest> requests;
        std::string nextCursor;
        std::atomic<bool> loading{false};
        std::mutex mutex;
        int selectedIdx{-1};
        Roblox::FriendDetail selectedDetail{};
        std::atomic<bool> detailsLoading{false};
    };

    struct AddFriendState {
        bool openPopup{false};
        char buffer[512]{};
        std::atomic<bool> loading{false};
    };

    FriendsState g_state;
    RequestsState g_requests;
    AddFriendState g_addFriend;

    constexpr std::string_view presenceIcon(std::string_view presence) noexcept {
        if (presence == "InStudio") return ICON_TOOL;
        if (presence == "InGame") return ICON_CONTROLLER;
        if (presence == "Online") return ICON_PERSON;
        return "";
    }

    std::string trim(std::string_view sv) {
        const auto start = sv.find_first_not_of(" \t\n\r");
        if (start == std::string_view::npos) return "";
        const auto end = sv.find_last_not_of(" \t\n\r");
        return std::string(sv.substr(start, end - start + 1));
    }

    std::string formatDisplayName(std::string_view displayName, std::string_view username) {
        if (displayName.empty() || displayName == username) {
            return std::string(username);
        }
        return std::string(displayName) + " (" + std::string(username) + ")";
    }

    float calculateComboWidth(const std::vector<std::string>& labels) {
        const auto& style = ImGui::GetStyle();
        const float maxWidth = std::ranges::max(labels | std::views::transform(
            [](const auto& lbl) { return ImGui::CalcTextSize(lbl.c_str()).x; }
        ));
        return maxWidth + style.FramePadding.x * 2.0f + ImGui::GetFrameHeight();
    }

    bool parseMultiUserInput(std::string_view input, std::vector<UserSpecifier>& outSpecs, std::string& error) {
        outSpecs.clear();
        const auto trimmed = trim(input);
        if (trimmed.empty()) return false;

        for (const auto token : std::views::split(trimmed, std::string_view(",\n\r"))) {
            const auto tokenStr = trim(std::string_view(token));
            if (tokenStr.empty()) continue;

            UserSpecifier spec{};
            if (!parseUserSpecifier(std::string(tokenStr), spec)) {
                error = "Invalid entry: " + std::string(tokenStr);
                return false;
            }
            outSpecs.push_back(spec);
        }
        return !outSpecs.empty();
    }

    void loadIncomingRequests(std::string_view cookie, bool reset) {
        if (g_requests.loading.load()) return;
        
        if (reset) {
            std::lock_guard lock(g_requests.mutex);
            g_requests.requests.clear();
            g_requests.nextCursor.clear();
        }

        g_requests.loading = true;
        std::string cursor;
        {
            std::lock_guard lock(g_requests.mutex);
            cursor = reset ? "" : g_requests.nextCursor;
        }

        ThreadTask::fireAndForget([cookieCopy = std::string(cookie), cursor]() {
            auto page = Roblox::getIncomingFriendRequests(cookieCopy, cursor, 100);
            {
                std::lock_guard lock(g_requests.mutex);
                g_requests.requests.insert(g_requests.requests.end(),
                    std::make_move_iterator(page.data.begin()),
                    std::make_move_iterator(page.data.end()));
                g_requests.nextCursor = std::move(page.nextCursor);
            }
            g_requests.loading = false;
        });
    }

    void renderAccountSelector(const AccountData& currentAccount) {
        std::vector<std::string> labels;
        labels.reserve(g_accounts.size());
        for (const auto& acc : g_accounts) {
            labels.push_back(formatDisplayName(acc.displayName, acc.username));
        }

        const float width = calculateComboWidth(labels);
        ImGui::SetNextItemWidth(width);
        ImGui::PushID("AccountSelector");

        const auto currentLabel = formatDisplayName(currentAccount.displayName, currentAccount.username);
        if (ImGui::BeginCombo("##AccountSelector", currentLabel.c_str())) {
            for (std::size_t idx = 0; idx < g_accounts.size(); ++idx) {
                const auto& acc = g_accounts[idx];
                const bool isSelected = (acc.id == g_state.viewAccountId);
                const bool disabled = !AccountFilters::IsAccountUsable(acc);
                
                ImGui::PushID(acc.id);
                if (disabled) ImGui::BeginDisabled(true);
                
                if (ImGui::Selectable(labels[idx].c_str(), isSelected) && !disabled) {
                    g_state.viewAccountId = acc.id;
                }
                
                if (disabled) ImGui::EndDisabled();
                if (isSelected) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }

    void renderViewModeSelector(const AccountData& account) {
        static constexpr const char* VIEW_MODES[] = {"Friends", "Requests"};
        
        const float width = calculateComboWidth({VIEW_MODES[0], VIEW_MODES[1]});
        ImGui::SetNextItemWidth(width);
        
        if (ImGui::Combo("##ViewMode", &g_state.viewMode, VIEW_MODES, IM_ARRAYSIZE(VIEW_MODES))) {
            if (g_state.lastViewMode != g_state.viewMode) {
                g_state.lastViewMode = g_state.viewMode;
                g_state.selectedFriendIdx = -1;
                g_state.selectedFriend = {};
                g_requests.selectedIdx = -1;
                
                if (g_state.viewMode == VIEW_MODE_REQUESTS) {
                    loadIncomingRequests(account.cookie, true);
                }
            }
        }
    }

    void renderAddFriendPopup(const AccountData& account) {
        if (g_addFriend.openPopup) {
            ImGui::OpenPopup("Add Friends");
            g_addFriend.openPopup = false;
        }

        if (!ImGui::BeginPopupModal("Add Friends", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            return;
        }

        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted("Enter one or more players, separated by commas or new lines. "
                              "Each entry can be a username or a userId (formatted as id=000).");
        ImGui::PopTextWrapPos();

        std::string validateErr;
        std::vector<UserSpecifier> specs;
        const auto trimmed = trim(g_addFriend.buffer);
        const bool hasInput = !trimmed.empty();
        const bool isValid = parseMultiUserInput(g_addFriend.buffer, specs, validateErr);
        const bool showError = hasInput && !isValid && !validateErr.empty();

        if (showError) {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
        }

        constexpr float MIN_WIDTH = 560.0f;
        const ImVec2 size(MIN_WIDTH, ImGui::GetTextLineHeight() * 5.0f + ImGui::GetStyle().FramePadding.y * 2.0f);
        ImGui::InputTextMultiline("##Input", g_addFriend.buffer, sizeof(g_addFriend.buffer), 
                                  size, ImGuiInputTextFlags_NoHorizontalScroll);

        if (showError) {
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }

        if (g_addFriend.loading.load()) {
            ImGui::SameLine();
            ImGui::TextUnformatted("Sending...");
        }

        ImGui::Spacing();

        const bool canSend = isValid && !specs.empty() && !g_addFriend.loading.load();
        ImGui::BeginDisabled(!canSend);
        const bool doSend = ImGui::Button("Send", ImVec2(80, 0));
        ImGui::EndDisabled();

        if (doSend) {
            g_addFriend.loading = true;
            ThreadTask::fireAndForget([specs, cookie = account.cookie]() {
                for (const auto& spec : specs) {
                    const uint64_t uid = spec.isId ? spec.id : Roblox::getUserIdFromUsername(spec.username);
                    std::string resp;
                    if (Roblox::sendFriendRequest(std::to_string(uid), cookie, &resp)) {
                        LOG_INFO("Friend request sent");
                    }
                }
                g_addFriend.loading = false;
            });
            g_addFriend.buffer[0] = '\0';
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0)) && !g_addFriend.loading.load()) {
            g_addFriend.buffer[0] = '\0';
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    void renderFriendContextMenu(const FriendInfo& frend, const AccountData& account) {
        if (!ImGui::BeginPopupContextItem("FriendContext")) return;

        if (ImGui::MenuItem("Copy Display Name")) {
            ImGui::SetClipboardText(frend.displayName.c_str());
        }
        if (ImGui::MenuItem("Copy Username")) {
            ImGui::SetClipboardText(frend.username.c_str());
        }
        if (ImGui::MenuItem("Copy User ID")) {
            ImGui::SetClipboardText(std::to_string(frend.id).c_str());
        }

        const bool inGame = frend.presence == "InGame" && frend.placeId && !frend.jobId.empty();
    	if (inGame) {
    		ImGui::Separator();
    		StandardJoinMenuParams menu{};
    		menu.placeId = frend.placeId;
    		menu.jobId = frend.jobId;

    		menu.onLaunchGame = [placeId = frend.placeId]() {
    			launchWithSelectedAccounts(LaunchParams::standard(placeId));
    		};

    		menu.onLaunchInstance = [placeId = frend.placeId, jobId = frend.jobId]() {
    			if (jobId.empty()) return;
    			launchWithSelectedAccounts(LaunchParams::gameJob(placeId, jobId));
    		};

            menu.onFillGame = [placeId = frend.placeId]() { FillJoinOptions(placeId, ""); };
            menu.onFillInstance = [frend]() { FillJoinOptions(frend.placeId, frend.jobId); };
            RenderStandardJoinMenu(menu);
        }

        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        if (ImGui::MenuItem("Unfriend")) {
            ModalPopup::AddYesNo(
                std::format("Unfriend {}?", frend.username),
                [frend, cookie = account.cookie, accountId = account.id]() {
                    ThreadTask::fireAndForget([frend, cookie, accountId]() {
                        std::string resp;
                        if (Roblox::unfriend(std::to_string(frend.id), cookie, &resp)) {
                            std::erase_if(g_friends, [&](const auto& f) { return f.id == frend.id; });
                            if (g_state.selectedFriendIdx >= 0 && 
                                g_state.selectedFriendIdx < static_cast<int>(g_friends.size()) &&
                                g_friends[g_state.selectedFriendIdx].id == frend.id) {
                                g_state.selectedFriendIdx = -1;
                                g_state.selectedFriend = {};
                            }
                            
                            std::erase_if(g_accountFriends[accountId], [&](const auto& f) { return f.id == frend.id; });
                            
                            auto& unfList = g_unfriendedFriends[accountId];
                            if (!std::ranges::contains(unfList, frend.id, &FriendInfo::id)) {
                                unfList.push_back(frend);
                            }
                            Data::SaveFriends();
                        }
                    });
                }
            );
        }
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }

    void renderFriendsList() {
        if (g_state.friendsLoading.load() && g_friends.empty()) {
            ImGui::Text("Loading friends...");
            return;
        }

        for (std::size_t idx = 0; idx < g_friends.size(); ++idx) {
            const auto& frend = g_friends[idx];
            ImGui::PushID(static_cast<int>(idx));

            std::string label;
            if (const auto icon = presenceIcon(frend.presence); !icon.empty()) {
                label += icon;
            }
            label += formatDisplayName(frend.displayName, frend.username);

            const ImVec4 color = getStatusColor(frend.presence);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            const bool clicked = ImGui::Selectable(label.c_str(), g_state.selectedFriendIdx == static_cast<int>(idx),
                                                   ImGuiSelectableFlags_SpanAllColumns);
            ImGui::PopStyleColor();

            if (const auto* acc = getAccountById(g_state.viewAccountId)) {
                renderFriendContextMenu(frend, *acc);
            }

            if (frend.presence == "InGame" && !frend.lastLocation.empty()) {
                const float indent = ImGui::GetStyle().FramePadding.x * 4.0f;
                ImGui::Indent(indent);
                ImVec4 gameColor = color;
                gameColor.x *= 0.75f;
                gameColor.y *= 0.75f;
                gameColor.z *= 0.75f;
                gameColor.w *= 0.65f;
                ImGui::PushStyleColor(ImGuiCol_Text, gameColor);
                ImGui::TextUnformatted(("\xEF\x83\x9A  " + frend.lastLocation).c_str());
                ImGui::PopStyleColor();
                ImGui::Unindent(indent);
            }

            if (clicked) {
                g_state.selectedFriendIdx = static_cast<int>(idx);
                if (g_state.selectedFriend.id != frend.id) {
                    g_state.selectedFriend = {};
                    if (const auto* acc = getAccountById(g_state.viewAccountId)) {
                        ThreadTask::fireAndForget(FriendsActions::FetchFriendDetails,
                                           std::to_string(frend.id),
                                           acc->cookie,
                                           std::ref(g_state.selectedFriend),
                                           std::ref(g_state.friendDetailsLoading));
                    }
                }
            }

            ImGui::PopID();
        }

        if (!g_state.unfriended.empty()) {
            ImGui::PushID("UnfriendedSection");
            ImGui::SeparatorText("Friends Lost");

            if (ImGui::BeginPopupContextItem("UnfriendedContext")) {
                if (ImGui::MenuItem("Clear")) {
                    g_state.unfriended.clear();
                    g_unfriendedFriends[g_state.viewAccountId].clear();
                    Data::SaveFriends();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();

            for (std::size_t idx = 0; idx < g_state.unfriended.size(); ++idx) {
                const auto& unfriend = g_state.unfriended[idx];
                ImGui::PushID(static_cast<int>(idx));
                const auto name = formatDisplayName(unfriend.displayName, unfriend.username);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                ImGui::TextUnformatted(name.c_str());
                ImGui::PopStyleColor();

                if (ImGui::BeginPopupContextItem("UnfriendedEntry")) {
                    if (ImGui::MenuItem("Copy Display Name")) {
                        ImGui::SetClipboardText(unfriend.displayName.c_str());
                    }
                    if (ImGui::MenuItem("Copy Username")) {
                        ImGui::SetClipboardText(unfriend.username.c_str());
                    }
                    if (ImGui::MenuItem("Copy User ID")) {
                        ImGui::SetClipboardText(std::to_string(unfriend.id).c_str());
                    }
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.85f, 0.4f, 1.0f));
                    if (ImGui::MenuItem("Add Friend")) {
                        if (const auto* acc = getAccountById(g_state.viewAccountId)) {
                            const uint64_t uid = unfriend.id;
                            const std::string cookie = acc->cookie;
                            ThreadTask::fireAndForget([uid, cookie]() {
                                std::string resp;
                                Roblox::sendFriendRequest(std::to_string(uid), cookie, &resp);
                            });
                        }
                    }
                    ImGui::PopStyleColor();
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
    }

    void renderFriendDetails() {
        constexpr float INDENT = 8.0f;

        if (g_state.selectedFriendIdx < 0 || g_state.selectedFriendIdx >= static_cast<int>(g_friends.size())) {
            ImGui::Indent(INDENT);
            ImGui::Spacing();
            ImGui::TextWrapped("Click a friend to see more details or take action.");
            ImGui::Unindent(INDENT);
            return;
        }

        if (g_state.friendDetailsLoading.load()) {
            ImGui::Indent(INDENT);
            ImGui::Spacing();
            ImGui::Text("Loading full details...");
            ImGui::Unindent(INDENT);
            return;
        }

        const auto& detail = g_state.selectedFriend;
        if (detail.id == 0) {
            ImGui::Indent(INDENT);
            ImGui::Spacing();
            ImGui::TextWrapped("Details not available.");
            ImGui::Unindent(INDENT);
            return;
        }

        constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerH | 
                                               ImGuiTableFlags_RowBg | 
                                               ImGuiTableFlags_SizingFixedFit;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 4.0f));

        const std::vector labels = {"Display Name:", "Username:", "User ID:", "Friends:", 
                                    "Followers:", "Following:", "Created:", "Description:"};
        const float labelWidth = std::max(ImGui::GetFontSize() * 7.5f, 
            std::ranges::max(labels | std::views::transform([](auto lbl) { 
                return ImGui::CalcTextSize(lbl).x; 
            })) + INDENT * 2.0f + ImGui::GetFontSize());

        if (ImGui::BeginTable("FriendDetails", 2, tableFlags)) {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

            auto addRow = [&](const char* label, const std::string& value, bool wrap = false) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Indent(INDENT);
                ImGui::Spacing();
                ImGui::TextUnformatted(label);
                ImGui::Spacing();
                ImGui::Unindent(INDENT);

                ImGui::TableSetColumnIndex(1);
                ImGui::Indent(INDENT);
                ImGui::Spacing();
                ImGui::PushID(label);
                if (wrap) {
                    ImGui::TextWrapped("%s", value.c_str());
                } else {
                    ImGui::TextUnformatted(value.c_str());
                }
                if (ImGui::BeginPopupContextItem("Copy")) {
                    if (ImGui::MenuItem("Copy")) {
                        ImGui::SetClipboardText(value.c_str());
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                ImGui::Spacing();
                ImGui::Unindent(INDENT);
            };

            addRow("Display Name:", detail.displayName.empty() ? detail.username : detail.displayName);
            addRow("Username:", detail.username);
            addRow("User ID:", std::to_string(detail.id));
            addRow("Friends:", std::to_string(detail.friends));
            addRow("Followers:", std::to_string(detail.followers));
            addRow("Following:", std::to_string(detail.following));
            addRow("Created:", formatAbsoluteWithRelativeFromIso(detail.createdIso));

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Indent(INDENT);
            ImGui::Spacing();
            ImGui::TextUnformatted("Description:");
            ImGui::Spacing();
            ImGui::Unindent(INDENT);

            ImGui::TableSetColumnIndex(1);
            ImGui::Indent(INDENT);
            ImGui::Spacing();

            const auto& style = ImGui::GetStyle();
            const float reserved = style.ItemSpacing.y * 2 + ImGui::GetFrameHeightWithSpacing();
            const float descHeight = std::max(ImGui::GetContentRegionAvail().y - reserved,
                                             ImGui::GetTextLineHeightWithSpacing() * 3.0f);

            const bool hasDesc = !detail.description.empty();
            const auto descStr = hasDesc ? detail.description : std::string("No description");
            
            ImGui::PushID("Description");
            ImGui::BeginChild("##DescScroll", ImVec2(0, descHeight - 4), false, ImGuiWindowFlags_HorizontalScrollbar);
            if (hasDesc) {
                ImGui::TextWrapped("%s", descStr.c_str());
            } else {
                ImGui::TextDisabled("%s", descStr.c_str());
            }
            if (ImGui::BeginPopupContextItem("CopyDesc")) {
                if (ImGui::MenuItem("Copy")) {
                    ImGui::SetClipboardText(descStr.c_str());
                }
                ImGui::EndPopup();
            }
            ImGui::EndChild();
            ImGui::PopID();

            ImGui::Spacing();
            ImGui::Unindent(INDENT);
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();

        ImGui::Separator();

        ImGui::Indent(INDENT / 2);
        const auto& frend = g_friends[g_state.selectedFriendIdx];
        const bool canJoin = frend.presence == "InGame" && frend.placeId && !frend.jobId.empty();

        ImGui::BeginDisabled(!canJoin);
    	if (ImGui::Button((std::string(ICON_JOIN) + " Launch Instance").c_str()) && canJoin) {
    		auto accountPtrs = getUsableSelectedAccounts();
    		if (accountPtrs.empty()) {
    			Status::Error("No usable accounts selected");
    			return;
    		}

    		// Copy for thread safety
    		std::vector<AccountData> accounts;
    		accounts.reserve(accountPtrs.size());
    		for (AccountData* acc : accountPtrs) {
    			accounts.push_back(*acc);
    		}

    		ThreadTask::fireAndForget([frend, accounts = std::move(accounts)]() {
				const uint64_t uid = frend.id;
				const auto pres = Roblox::getPresences({uid}, accounts.front().cookie);
				const auto it = pres.find(uid);
				if (it == pres.end() || it->second.presence != "InGame" ||
					it->second.placeId == 0 || it->second.jobId.empty()) {
					Status::Error("User is not joinable");
					return;
				}
				launchWithAccounts(LaunchParams::followUser(std::to_string(uid)), accounts);
			});
    	}
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button((std::string(ICON_OPEN_LINK) + " Open Page").c_str())) {
            ImGui::OpenPopup("ProfileContext");
        }
        ImGui::OpenPopupOnItemClick("ProfileContext");

        if (ImGui::BeginPopup("ProfileContext")) {
        	std::string cookie;
        	std::string userId;

        	if (!g_selectedAccountIds.empty()) {
        		if (const AccountData* acc = getAccountById(*g_selectedAccountIds.begin())) {
        			cookie = acc->cookie;
        			userId = acc->userId;
        		}
        	}
            const auto uidStr = std::to_string(detail.id);

            if (ImGui::MenuItem("Profile")) {
                LaunchWebviewImpl("https://www.roblox.com/users/" + uidStr + "/profile",
                            "Roblox Profile", cookie, userId);
            }
            if (ImGui::MenuItem("Friends")) {
                LaunchWebviewImpl("https://www.roblox.com/users/" + uidStr + "/friends",
                            "Friends", cookie, userId);
            }
            if (ImGui::MenuItem("Favorites")) {
                LaunchWebviewImpl("https://www.roblox.com/users/" + uidStr + "/favorites",
                            "Favorites", cookie, userId);
            }
            if (ImGui::MenuItem("Inventory")) {
                LaunchWebviewImpl("https://www.roblox.com/users/" + uidStr + "/inventory/#!/accessories",
                            "Inventory", cookie, userId);
            }
            if (ImGui::MenuItem("Rolimons")) {
                LaunchWebviewImpl("https://www.rolimons.com/player/" + uidStr, "Rolimons");
            }
            ImGui::EndPopup();
        }
        ImGui::Unindent(INDENT / 2);
    }

}

void RenderFriendsTab() {
    if (g_selectedAccountIds.empty()) {
        ImGui::TextDisabled("Select an account in the Accounts tab to view its friends.");
        return;
    }

    const auto isValidViewAccount = [](const AccountData& a) {
        return a.id == g_state.viewAccountId && AccountFilters::IsAccountUsable(a);
    };

    if (g_state.viewAccountId == -1 || !std::ranges::any_of(g_accounts, isValidViewAccount)) {
        g_state.viewAccountId = -1;
        
        for (const int id : g_selectedAccountIds) {
            if (AccountData* acc = getAccountById(id)) {
                if (AccountFilters::IsAccountUsable(*acc)) {
                    g_state.viewAccountId = acc->id;
                    break;
                }
            }
        }

        if (g_state.viewAccountId == -1) {
            if (const auto it = std::ranges::find_if(g_accounts,
                [](const auto& a) { return AccountFilters::IsAccountUsable(a); });
                it != g_accounts.end()) {
                g_state.viewAccountId = it->id;
            }
        }
    }

    AccountData* account = getAccountById(g_state.viewAccountId);
    if (!account) {
        ImGui::TextDisabled("Selected account not found.");
        return;
    }

    g_state.unfriended = g_unfriendedFriends[g_state.viewAccountId];

    if (g_state.viewAccountId != g_state.lastAccountId) {
        g_friends.clear();
        g_state.selectedFriendIdx = -1;
        g_state.selectedFriend = {};
        g_state.friendsLoading = false;
        g_state.friendDetailsLoading = false;
        g_state.unfriended = g_unfriendedFriends[g_state.viewAccountId];
        g_state.lastAccountId = g_state.viewAccountId;
        
        {
            std::lock_guard lock(g_requests.mutex);
            g_requests.requests.clear();
            g_requests.nextCursor.clear();
        }
        g_requests.loading = false;
        g_requests.selectedIdx = -1;
        g_requests.selectedDetail = {};
        g_requests.detailsLoading = false;

        if (!account->userId.empty()) {
            ThreadTask::fireAndForget(FriendsActions::RefreshFullFriendsList,
                               account->id, account->userId, account->cookie,
                               std::ref(g_friends), std::ref(g_state.friendsLoading));
            
            if (g_state.viewMode == VIEW_MODE_REQUESTS) {
                loadIncomingRequests(account->cookie, true);
            }
        }
    }

    renderAccountSelector(*account);
    ImGui::SameLine();
    renderViewModeSelector(*account);

    const bool isLoading = g_state.friendsLoading.load() || 
                          (g_state.viewMode == VIEW_MODE_REQUESTS && g_requests.loading.load());
    ImGui::BeginDisabled(isLoading);
    
    if (ImGui::Button((std::string(ICON_REFRESH) + " Refresh").c_str()) && !account->userId.empty()) {
        g_state.selectedFriendIdx = -1;
        g_state.selectedFriend = {};
        
        if (g_state.viewMode == VIEW_MODE_FRIENDS) {
            ThreadTask::fireAndForget(FriendsActions::RefreshFullFriendsList,
                               account->id, account->userId, account->cookie,
                               std::ref(g_friends), std::ref(g_state.friendsLoading));
        } else {
            loadIncomingRequests(account->cookie, true);
        }
    }
    
    ImGui::SameLine();
    if (ImGui::Button((std::string(ICON_USER_PLUS) + " Add Friends").c_str())) {
        g_addFriend.openPopup = true;
    }
    ImGui::EndDisabled();

    renderAddFriendPopup(*account);

    constexpr float MIN_LIST_WIDTH = 224.0f;
    constexpr float MAX_LIST_WIDTH = 320.0f;
    const float availWidth = ImGui::GetContentRegionAvail().x;
    const float listWidth = std::clamp(availWidth * 0.28f, MIN_LIST_WIDTH, MAX_LIST_WIDTH);

    ImGui::BeginChild("##FriendsList", ImVec2(listWidth, 0), true);
    renderFriendsList();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##Details", ImVec2(0, 0), true);
    ImGui::PopStyleVar();
    renderFriendDetails();
    ImGui::EndChild();
}