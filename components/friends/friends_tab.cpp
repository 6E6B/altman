#include "friends.h"
#include <imgui.h>
#include <algorithm>
#include <vector>
#include <string>
#include <atomic>
#include <utility>
#include <cctype>


#include "../data.h"
#include "../../utils/network/roblox_api.h"
#include "../../utils/system/launcher.hpp"
#include "../../utils/threads/threading.h"
#include "./friends_actions.h"
#include "../utils/network/webview.hpp"
#include "../games/games_utils.h"
#include "../../utils/gui/confirm.h"

using namespace ImGui;
using namespace std;

static int g_selectedFriendIdx = -1;
static RobloxApi::FriendDetail g_selectedFriend;
static atomic<bool> g_friendDetailsLoading{false};
static atomic<bool> g_friendsLoading{false};
static vector<FriendInfo> g_unfriended;

static int g_lastAcctIdForFriends = -1;

static auto ICON_TOOL = "\xEF\x82\xAD ";
static auto ICON_PERSON = "\xEF\x80\x87 ";
static auto ICON_CONTROLLER = "\xEF\x84\x9B ";
static auto ICON_REFRESH = "\xEF\x8B\xB1 ";
static auto ICON_OPEN_LINK = "\xEF\x8A\xBB ";
static auto ICON_INVENTORY = "\xEF\x8A\x90 ";
static auto ICON_JOIN = "\xEF\x8B\xB6 ";
static auto ICON_USER_PLUS = "\xEF\x88\xB4 ";

static bool s_openAddFriendPopup = false;
static char s_addFriendBuffer[64] = "";
static atomic<bool> s_addFriendLoading{false};

static inline string trim_copy(string s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    size_t end = s.find_last_not_of(" \t\n\r");
    if (start == string::npos) return "";
    return s.substr(start, end - start + 1);
}

static const char *presenceIcon(const string &p) {
    if (p == "InStudio")
        return ICON_TOOL;
    if (p == "InGame")
        return ICON_CONTROLLER;
    if (p == "Online")
        return ICON_PERSON;
    return "";
}


void RenderFriendsTab() {
    if (g_selectedAccountIds.empty()) {
        TextDisabled("Select an account in the Accounts tab to view its friends.");
        return;
    }
    int currentAcctId = *g_selectedAccountIds.begin();
    auto it = find_if(g_accounts.begin(), g_accounts.end(),
                      [&](auto &a) {
                          return a.id == currentAcctId;
                      });
    if (it == g_accounts.end()) {
        TextDisabled("Selected account not found.");
        return;
    }
    const AccountData &acct = *it;
    g_unfriended = g_unfriendedFriends[currentAcctId];

    if (currentAcctId != g_lastAcctIdForFriends) {
        g_friends.clear();
        g_selectedFriendIdx = -1;
        g_selectedFriend = {};
        g_friendsLoading = false;
        g_friendDetailsLoading = false;
        g_unfriended = g_unfriendedFriends[currentAcctId];
        g_lastAcctIdForFriends = currentAcctId;

        if (!acct.userId.empty()) {
            Threading::newThread(FriendsActions::RefreshFullFriendsList, acct.id, acct.userId, acct.cookie,
                                 ref(g_friends),
                                 ref(g_friendsLoading));
        }
    }

    BeginDisabled(g_friendsLoading.load());
    if (Button((string(ICON_REFRESH) + " Refresh").c_str()) && !acct.userId.empty()) {
        g_selectedFriendIdx = -1;
        g_selectedFriend = {};
        Threading::newThread(FriendsActions::RefreshFullFriendsList, acct.id, acct.userId, acct.cookie, ref(g_friends),
                             ref(g_friendsLoading));
    }
    SameLine();
    if (Button((string(ICON_USER_PLUS) + " Add Friend").c_str())) {
        s_openAddFriendPopup = true;
    }
    EndDisabled();

    if (s_openAddFriendPopup) {
        OpenPopup("Add Friend");
        s_openAddFriendPopup = false;
    }
    if (BeginPopupModal("Add Friend", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        float inputW = GetContentRegionAvail().x;
        if (inputW < 100.0f)
            inputW = 100.0f;
        PushItemWidth(inputW);
        InputTextWithHint("##AddFriendUser", "Username or ID", s_addFriendBuffer, sizeof(s_addFriendBuffer));
        PopItemWidth();
        if (s_addFriendLoading.load()) {
            SameLine();
            TextUnformatted("Sending...");
        }
        Spacing();
        float sendWidth = CalcTextSize("Send").x + GetStyle().FramePadding.x * 2.0f;
        float cancelWidth = CalcTextSize("Cancel").x + GetStyle().FramePadding.x * 2.0f;
        if (Button("Send", ImVec2(sendWidth, 0)) && s_addFriendBuffer[0] != '\0' && !s_addFriendLoading.load()) {
            string input = trim_copy(s_addFriendBuffer);
            s_addFriendLoading = true;
            Threading::newThread([input, cookie = acct.cookie]() {
                try {
                    uint64_t uid = 0;
                    if (input.empty()) throw runtime_error("Username not provided");
                    if (all_of(input.begin(), input.end(), [](unsigned char c) { return std::isdigit(c); })) {
                        uid = stoull(input);
                    } else {
                        uid = RobloxApi::getUserIdFromUsername(input);
                    }
                    string resp;
                    bool ok = RobloxApi::sendFriendRequest(to_string(uid), cookie, &resp);
                    if (ok) {
                        LOG_INFO("Friend request sent");
                        cerr << "Friend request response: " << resp << "\n";
                    } else {
                        cerr << "Friend request failed: " << resp << "\n";
                        LOG_INFO("Friend request failed");
                    }
                } catch (const exception &e) {
                    cerr << "Friend request exception: " << e.what() << "\n";
                    LOG_INFO(e.what());
                }
                s_addFriendLoading = false;
            });
            s_addFriendBuffer[0] = '\0';
            CloseCurrentPopup();
        }
        SameLine(0, GetStyle().ItemSpacing.x);
        if (Button("Cancel", ImVec2(cancelWidth, 0)) && !s_addFriendLoading.load()) {
            s_addFriendBuffer[0] = '\0';
            CloseCurrentPopup();
        }
        EndPopup();
    }

    float friendsListWidth = 300.0f;

    BeginChild("##FriendsList", ImVec2(friendsListWidth, 0), true);
    if (g_friendsLoading.load() && g_friends.empty()) {
        Text("Loading friends...");
    } else {
        for (size_t i = 0; i < g_friends.size(); ++i) {
            const auto &f = g_friends[i];
            PushID(static_cast<int>(i));

            string label;
            const char *icon = presenceIcon(f.presence);
            if (*icon) label += icon;
            label += (f.displayName == f.username || f.displayName.empty())
                         ? f.username
                         : (f.displayName + " (" + f.username + ")");

            ImVec4 txtCol = getStatusColor(f.presence);
            PushStyleColor(ImGuiCol_Text, txtCol);

            bool clicked = Selectable(label.c_str(),
                                      g_selectedFriendIdx == static_cast<int>(i),
                                      ImGuiSelectableFlags_SpanAllColumns);
            PopStyleColor();

            if (BeginPopupContextItem("FriendRowContextMenu")) {
                if (MenuItem("Copy Display Name")) {
                    SetClipboardText(f.displayName.c_str());
                }
                if (MenuItem("Copy Username")) {
                    SetClipboardText(f.username.c_str());
                }
                if (MenuItem("Copy User ID")) {
                    string idStr = to_string(f.id);
                    SetClipboardText(idStr.c_str());
                }
                bool inGame = f.presence == "InGame" && f.placeId && !f.gameId.empty();
                if (inGame) {
                    Separator();
                    if (MenuItem("Join")) {
                        vector<pair<int, string> > accounts;
                        for (int id: g_selectedAccountIds) {
                            auto itA = find_if(g_accounts.begin(), g_accounts.end(), [&](const AccountData &a) {
                                return a.id == id;
                            });
                            if (itA != g_accounts.end())
                                accounts.emplace_back(itA->id, itA->cookie);
                        }
                        if (!accounts.empty()) {
                            Threading::newThread([row = f, accounts]() {
                                launchRobloxSequential(row.placeId, row.gameId, accounts);
                            });
                        }
                    }
                    if (MenuItem("Copy Place ID")) {
                        SetClipboardText(to_string(f.placeId).c_str());
                    }
                    if (MenuItem("Copy Job ID")) {
                        SetClipboardText(f.gameId.c_str());
                    }
                    if (BeginMenu("Copy Launch Method")) {
                        if (MenuItem("Browser Link")) {
                            string link = "https://www.roblox.com/games/start?placeId=" + to_string(f.placeId) +
                                          "&gameInstanceId=" + f.gameId;
                            SetClipboardText(link.c_str());
                        }

                        char buf[256];
                        snprintf(buf, sizeof(buf), "roblox://placeId=%llu&gameInstanceId=%s",
                                 (unsigned long long) f.placeId, f.gameId.c_str());
                        if (MenuItem("Deep Link")) SetClipboardText(buf);
                        string js = "Roblox.GameLauncher.joinGameInstance(" + to_string(f.placeId) + ", \"" + f.gameId +
                                    "\")";
                        if (MenuItem("JavaScript")) SetClipboardText(js.c_str());
                        string luau = "game:GetService(\"TeleportService\"):TeleportToPlaceInstance(" +
                                      to_string(f.placeId) + ", \"" + f.gameId + "\")";
                        if (MenuItem("ROBLOX Luau")) SetClipboardText(luau.c_str());
                        ImGui::EndMenu();
                    }
                }
                Separator();
                PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
                if (MenuItem("Unfriend")) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "Unfriend %s?", f.username.c_str());
                    FriendInfo fCopy = f;
                    uint64_t friendId = fCopy.id;
                    string cookieCopy = acct.cookie;
                    int acctIdCopy = acct.id;
                    ConfirmPopup::Add(buf, [fCopy, friendId, cookieCopy, acctIdCopy]() {
                        Threading::newThread([fCopy, friendId, cookieCopy, acctIdCopy]() {
                            string resp;
                            bool ok = RobloxApi::unfriend(to_string(friendId), cookieCopy, &resp);
                            if (ok) {
                                erase_if(g_friends, [&](const FriendInfo &fi) { return fi.id == friendId; });
                                if (g_selectedFriendIdx >= 0 && g_selectedFriendIdx < static_cast<int>(g_friends.size())
                                    &&
                                    g_friends[g_selectedFriendIdx].id == friendId) {
                                    g_selectedFriendIdx = -1;
                                    g_selectedFriend = {};
                                }
                                erase_if(g_accountFriends[acctIdCopy], [&](const FriendInfo &fi) {
                                    return fi.id == friendId;
                                });
                                auto &unfList = g_unfriendedFriends[acctIdCopy];
                                if (std::none_of(unfList.begin(), unfList.end(), [&](const FriendInfo &fi) {
                                    return fi.id == friendId;
                                }))
                                    unfList.push_back(fCopy);
                                Data::SaveFriends();
                            } else {
                                cerr << "Unfriend failed: " << resp << "\n";
                            }
                        });
                    });
                }
                PopStyleColor();
                EndPopup();
            }

            if (f.presence == "InGame" && !f.lastLocation.empty()) {
                const float indent = GetStyle().FramePadding.x * 4.0f;
                Indent(indent);
                ImVec4 gameCol = txtCol;
                gameCol.x *= 0.75f;
                gameCol.y *= 0.75f;
                gameCol.z *= 0.75f;
                gameCol.w *= 0.65f;

                PushStyleColor(ImGuiCol_Text, gameCol);
                TextUnformatted(string("\xEF\x83\x9A  " + f.lastLocation).c_str());
                PopStyleColor();

                Unindent(indent);
            }


            if (clicked) {
                g_selectedFriendIdx = static_cast<int>(i);
                if (g_selectedFriend.id != f.id) {
                    g_selectedFriend = {};
                    Threading::newThread(FriendsActions::FetchFriendDetails,
                                         to_string(f.id),
                                         acct.cookie,
                                         ref(g_selectedFriend),
                                         ref(g_friendDetailsLoading));
                }
            }
            PopID();
        }

        if (!g_unfriended.empty()) {
            Separator();
            PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
            TextUnformatted("Friends Lost:");
            for (const auto &uf: g_unfriended) {
                string name = uf.displayName.empty() || uf.displayName == uf.username
                                  ? uf.username
                                  : uf.displayName + " (" + uf.username + ")";
                TextUnformatted(name.c_str());
            }
            PopStyleColor();
        }
    }
    EndChild();

    SameLine();

    float desiredTextIndent = 8.0f;
    PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    BeginChild("##FriendDetails", ImVec2(0, 0), true);
    PopStyleVar();

    if (g_selectedFriendIdx < 0 || g_selectedFriendIdx >= static_cast<int>(g_friends.size())) {
        Indent(desiredTextIndent);
        Spacing();
        TextWrapped("Click a friend to see more details or take action.");
        Unindent(desiredTextIndent);
    } else if (g_friendDetailsLoading.load()) {
        Indent(desiredTextIndent);
        Spacing();
        Text("Loading full details...");
        Unindent(desiredTextIndent);
    } else {
        const auto &D = g_selectedFriend;
        if (D.id == 0 && g_friends[g_selectedFriendIdx].id != 0) {
            Indent(desiredTextIndent);
            Spacing();
            Text("Fetching details for %s...", g_friends[g_selectedFriendIdx].username.c_str());

            Unindent(desiredTextIndent);
        } else if (D.id == 0) {
            Indent(desiredTextIndent);
            Spacing();
            TextWrapped("Details not available or selection issue.");
            Unindent(desiredTextIndent);
        } else {
            ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                         ImGuiTableFlags_SizingFixedFit;
            PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 4.0f));
            if (BeginTable("FriendInfoTable", 2, tableFlags)) {
                TableSetupColumn("##friendlabel", ImGuiTableColumnFlags_WidthFixed, 120.f);
                TableSetupColumn("##friendvalue", ImGuiTableColumnFlags_WidthStretch);

                auto addFriendDataRow = [&](const char *label, const string &value, bool isWrapped = false) {
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
                    if (isWrapped) {
                        TextWrapped("%s", value.c_str());
                    } else {
                        TextUnformatted(value.c_str());
                    }
                    if (BeginPopupContextItem("CopyFriendValue")) {
                        if (MenuItem("Copy")) {
                            SetClipboardText(value.c_str());
                        }
                        EndPopup();
                    }
                    PopID();
                    Spacing();
                    Unindent(desiredTextIndent);
                };

                auto addFriendDataRowInt = [&](const char *label, int value) {
                    addFriendDataRow(label, to_string(value));
                };

                addFriendDataRow("User ID:", to_string(D.id));
                addFriendDataRow("Username:", D.username);
                addFriendDataRow("Display Name:", D.displayName.empty() ? D.username : D.displayName);
                addFriendDataRow("Created:", formatPrettyDate(D.createdIso));
                addFriendDataRowInt("Followers:", D.followers);
                addFriendDataRowInt("Following:", D.following);

                TableNextRow();
                TableSetColumnIndex(0);
                Indent(desiredTextIndent);
                Spacing();
                TextUnformatted("Description:");
                Spacing();
                Unindent(desiredTextIndent);

                TableSetColumnIndex(1);
                Indent(desiredTextIndent);
                Spacing();

                ImGuiStyle &style = GetStyle();

                float spaceForBottomSpacingInCell = style.ItemSpacing.y;

                float spaceForSeparator = style.ItemSpacing.y;

                float spaceForButtons = GetFrameHeightWithSpacing();

                float reservedHeightBelowDescContent = spaceForBottomSpacingInCell + spaceForSeparator +
                                                       spaceForButtons;

                float availableHeightForDescAndBelow = GetContentRegionAvail().y;

                float descChildHeight = availableHeightForDescAndBelow - reservedHeightBelowDescContent;

                float minDescHeight = GetTextLineHeightWithSpacing() * 3.0f;
                if (descChildHeight < minDescHeight) {
                    descChildHeight = minDescHeight;
                }

                const string descStr = D.description.empty() ? "(No description)" : D.description;
                PushID("FriendDesc");
                BeginChild("##FriendDescScroll", ImVec2(0, descChildHeight - 4), false,
                           ImGuiWindowFlags_HorizontalScrollbar);
                TextWrapped("%s", descStr.c_str());
                if (BeginPopupContextItem("CopyFriendDesc")) {
                    if (MenuItem("Copy")) {
                        SetClipboardText(descStr.c_str());
                    }
                    EndPopup();
                }
                EndChild();
                PopID();

                Spacing();
                Unindent(desiredTextIndent);

                EndTable();
            }
            PopStyleVar();

            Separator();

            Indent(desiredTextIndent / 2);
            const FriendInfo &row = g_friends[g_selectedFriendIdx];

            bool canJoin = (row.presence == "InGame" && row.placeId && !row.gameId.empty());
            BeginDisabled(!canJoin);
            if (Button((string(ICON_JOIN) + " Join Game").c_str()) && canJoin) {
                vector<pair<int, string> > accounts;
                for (int id: g_selectedAccountIds) {
                    auto it = find_if(g_accounts.begin(), g_accounts.end(),
                                      [&](const AccountData &a) { return a.id == id; });
                    if (it != g_accounts.end())
                        accounts.emplace_back(it->id, it->cookie);
                }
                if (!accounts.empty()) {
                    Threading::newThread([row, accounts]() {
                        launchRobloxSequential(row.placeId, row.gameId, accounts);
                    });
                }
            }

            EndDisabled();
            SameLine();
            if (Button((string(ICON_OPEN_LINK) + " Open Profile").c_str())) {
                if (D.id)
                    LaunchWebview(
                        "https://www.roblox.com/users/" + to_string(D.id) + "/profile",
                        "Roblox Profile", acct.cookie);
            }
            SameLine();
            if (Button((string(ICON_INVENTORY) + " Open Inventory").c_str())) {
                if (D.id)
                    LaunchWebview(
                        "https://www.roblox.com/users/" + to_string(D.id) + "/inventory/#!/accessories",
                        "Roblox Inventory", acct.cookie);
            }
            Unindent(desiredTextIndent / 2);
        }
    }

    EndChild();
}
