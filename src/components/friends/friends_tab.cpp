#include "friends.h"
#include <imgui.h>
#include <algorithm>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <utility>
#include <cctype>
#include <iterator>

#include "../data.h"
#include "network/roblox.h"
#include "system/launcher.hpp"
#include "system/threading.h"
#include "./friends_actions.h"
#include "ui/webview.hpp"
#include "../games/games_utils.h"
#include "core/time_utils.h"
#include "ui/confirm.h"
#include "../accounts/accounts_join_ui.h"
#include "../context_menus.h"
#include "../../utils/core/account_utils.h"

using namespace ImGui;
using namespace std;

template <typename Container, typename Pred>
static inline void erase_if_local(Container &c, Pred p)
{
    c.erase(remove_if(c.begin(), c.end(), p), c.end());
}

static int g_selectedFriendIdx = -1;
static Roblox::FriendDetail g_selectedFriend;
static atomic<bool> g_friendDetailsLoading{false};
static atomic<bool> g_friendsLoading{false};
static vector<FriendInfo> g_unfriended;

static int g_lastAcctIdForFriends = -1;

// Account whose friends list is currently being viewed. Defaults to the first
// selected account but can be changed via the UI combo box.
static int g_viewAcctId = -1;

// Friends tab view mode: 0 = Friends, 1 = Requests
static int g_friendsViewMode = 0;
static vector<Roblox::IncomingFriendRequest> g_incomingRequests;
static string g_incomingReqNextCursor;
static atomic<bool> g_incomingRequestsLoading{false};
static std::mutex g_incomingReqMutex; // protects g_incomingRequests and g_incomingReqNextCursor
static int g_lastViewMode = -1;
static int g_selectedRequestIdx = -1;
static Roblox::FriendDetail g_selectedRequestDetail;
static atomic<bool> g_requestDetailsLoading{false};

static inline void LoadIncomingRequests(const string &cookie, bool reset)
{
    if (g_incomingRequestsLoading.load()) return;
    if (reset) {
        std::lock_guard<std::mutex> lk(g_incomingReqMutex);
        g_incomingRequests.clear();
        g_incomingReqNextCursor.clear();
    }
    g_incomingRequestsLoading = true;
    string cursor;
    if (reset) {
        cursor = "";
    } else {
        std::lock_guard<std::mutex> lk(g_incomingReqMutex);
        cursor = g_incomingReqNextCursor;
    }
    Threading::newThread([cookie, cursor]() {
        auto page = Roblox::getIncomingFriendRequests(cookie, cursor, 100);
        {
            std::lock_guard<std::mutex> lk(g_incomingReqMutex);
            for (auto &r : page.data) g_incomingRequests.push_back(std::move(r));
            g_incomingReqNextCursor = page.nextCursor;
        }
        g_incomingRequestsLoading = false;
    });
}

static auto ICON_TOOL = "\xEF\x82\xAD ";
static auto ICON_PERSON = "\xEF\x80\x87 ";
static auto ICON_CONTROLLER = "\xEF\x84\x9B ";
static auto ICON_REFRESH = "\xEF\x8B\xB1 ";
static auto ICON_OPEN_LINK = "\xEF\x8A\xBB ";
static auto ICON_INVENTORY = "\xEF\x8A\x90 ";
static auto ICON_JOIN = "\xEF\x8B\xB6 ";
static auto ICON_USER_PLUS = "\xEF\x88\xB4 ";

static bool s_openAddFriendPopup = false;
static char s_addFriendBuffer[512] = "";
static atomic<bool> s_addFriendLoading{false};

static inline string trim_copy(string s)
{
    size_t start = s.find_first_not_of(" \t\n\r");
    size_t end = s.find_last_not_of(" \t\n\r");
    if (start == string::npos)
        return "";
    return s.substr(start, end - start + 1);
}

static inline bool parseMultiUserInput(const string &input, vector<UserSpecifier> &outSpecs, string &error)
{
    outSpecs.clear();
    string s = trim_copy(input);
    if (s.empty())
    {
        // No input: neutral (not valid to send, but not an error)
        return false;
    }
    size_t start = 0;
    while (start <= s.size())
    {
        size_t delim = s.find_first_of(",\n\r", start);
        string token = delim == string::npos ? s.substr(start) : s.substr(start, delim - start);
        token = trim_copy(token);
        if (token.empty())
        {
            // Skip empty tokens (e.g., extra commas or spaces)
            if (delim == string::npos) break;
            start = delim + 1;
            continue;
        }
        UserSpecifier spec{};
        if (!parseUserSpecifier(token, spec))
        {
            error = "Invalid entry: " + token;
            return false;
        }
        outSpecs.push_back(spec);
        if (delim == string::npos)
            break;
        start = delim + 1;
    }
    return !outSpecs.empty();
}

static const char *presenceIcon(const string &p)
{
    if (p == "InStudio")
        return ICON_TOOL;
    if (p == "InGame")
        return ICON_CONTROLLER;
    if (p == "Online")
        return ICON_PERSON;
    return "";
}

void RenderFriendsTab()
{
    if (g_selectedAccountIds.empty())
    {
        TextDisabled("Select an account in the Accounts tab to view its friends.");
        return;
    }

    // Ensure the currently viewed account is valid and not banned-like.
    auto isCurrentViewAccount = [&](const AccountData &a)
    {
        return a.id == g_viewAcctId && AccountFilters::IsAccountUsable(a);
    };

    if (g_viewAcctId == -1 || std::none_of(g_accounts.begin(), g_accounts.end(), isCurrentViewAccount))
    {
    // Prefer a selected, non-banned-like account if one exists
        g_viewAcctId = -1;
        for (int id : g_selectedAccountIds)
        {
            auto itSel = std::find_if(g_accounts.begin(), g_accounts.end(), [&](const AccountData &a)
                      { return a.id == id && AccountFilters::IsAccountUsable(a); });
            if (itSel != g_accounts.end())
            {
                g_viewAcctId = id;
                break;
            }
        }
    // Fallback to the first non-banned-like account in the list
        if (g_viewAcctId == -1)
        {
            auto itFirst = std::find_if(g_accounts.begin(), g_accounts.end(), [&](const AccountData &a)
                    { return AccountFilters::IsAccountUsable(a); });
            if (itFirst != g_accounts.end())
                g_viewAcctId = itFirst->id;
        }
    }

    int currentAcctId = g_viewAcctId;
    auto it = find_if(g_accounts.begin(), g_accounts.end(),
                      [&](auto &a)
                      {
                          return a.id == currentAcctId;
                      });
    if (it == g_accounts.end())
    {
        TextDisabled("Selected account not found.");
        return;
    }
    const AccountData &acct = *it;
    g_unfriended = g_unfriendedFriends[currentAcctId];

    if (currentAcctId != g_lastAcctIdForFriends)
    {
        g_friends.clear();
        g_selectedFriendIdx = -1;
        g_selectedFriend = {};
        g_friendsLoading = false;
        g_friendDetailsLoading = false;
        g_unfriended = g_unfriendedFriends[currentAcctId];
        g_lastAcctIdForFriends = currentAcctId;
        {
            std::lock_guard<std::mutex> lk(g_incomingReqMutex);
            g_incomingRequests.clear();
            g_incomingReqNextCursor.clear();
        }
        g_incomingRequestsLoading = false;
    g_selectedRequestIdx = -1;
    g_selectedRequestDetail = {};
    g_requestDetailsLoading = false;

        if (!acct.userId.empty())
        {
            Threading::newThread(FriendsActions::RefreshFullFriendsList, acct.id, acct.userId, acct.cookie,
                                 ref(g_friends),
                                 ref(g_friendsLoading));
            if (g_friendsViewMode == 1)
                LoadIncomingRequests(acct.cookie, true);
        }
    }
    {
        float maxLabelWidth = 0.0f;
    for (const auto &acc : g_accounts)
    {
            string labelStr;
            if (acc.displayName == acc.username || acc.displayName.empty()) {
                labelStr = acc.username;
            } else {
                labelStr = acc.displayName + " (" + acc.username + ")";
            }
            float w = CalcTextSize(labelStr.c_str()).x;
            if (w > maxLabelWidth)
                maxLabelWidth = w;
        }
        ImGuiStyle &style = GetStyle();

        float comboWidth = maxLabelWidth + style.FramePadding.x * 2.0f + GetFrameHeight();

        SetNextItemWidth(comboWidth);

        PushID("AccountSelectorCombo");
        string currentLabelStr;
        if (acct.displayName == acct.username || acct.displayName.empty()) {
            currentLabelStr = acct.username;
        } else {
            currentLabelStr = acct.displayName + " (" + acct.username + ")";
        }
        if (BeginCombo("##AccountSelector", currentLabelStr.c_str()))
        {
            for (const auto &acc : g_accounts)
            {
                string labelStr;
                if (acc.displayName == acc.username || acc.displayName.empty()) {
                    labelStr = acc.username;
                } else {
                    labelStr = acc.displayName + " (" + acc.username + ")";
                }
                bool isSelected = (acc.id == g_viewAcctId);
                // Push a unique ID for each account item in the dropdown
                PushID(acc.id);
                bool disabled = !AccountFilters::IsAccountUsable(acc);
                if (disabled)
                    BeginDisabled(true);
                if (Selectable(labelStr.c_str(), isSelected))
                {
                    if (!disabled)
                        g_viewAcctId = acc.id;
                }
                if (disabled)
                    EndDisabled();
                // Pop the unique ID
                PopID();
                if (isSelected)
                    SetItemDefaultFocus();
            }
            EndCombo();
        }
        PopID();
    }

    SameLine();

    // Friends | Requests selector next to account selection
    {
        static const char* kViewModes[] = {"Friends", "Requests"};
        ImGuiStyle &style2 = GetStyle();
        float maxModeLabel = 0.0f;
        for (int i = 0; i < IM_ARRAYSIZE(kViewModes); ++i) {
            float w = CalcTextSize(kViewModes[i]).x;
            if (w > maxModeLabel) maxModeLabel = w;
        }
        float modeComboWidth = maxModeLabel + style2.FramePadding.x * 2.0f + GetFrameHeight();
        SetNextItemWidth(modeComboWidth);
        Combo("##FriendsViewMode", &g_friendsViewMode, kViewModes, IM_ARRAYSIZE(kViewModes));
        if (g_lastViewMode != g_friendsViewMode) {
            g_lastViewMode = g_friendsViewMode;
            g_selectedFriendIdx = -1;
            g_selectedFriend = {};
            g_selectedRequestIdx = -1;
            if (g_friendsViewMode == 1) {
                LoadIncomingRequests(acct.cookie, true);
            }
        }
    }

    BeginDisabled(g_friendsLoading.load() || (g_friendsViewMode == 1 && g_incomingRequestsLoading.load()));
    if (Button((string(ICON_REFRESH) + " Refresh").c_str()) && !acct.userId.empty())
    {
        g_selectedFriendIdx = -1;
        g_selectedFriend = {};
        if (g_friendsViewMode == 0) {
            Threading::newThread(FriendsActions::RefreshFullFriendsList, acct.id, acct.userId, acct.cookie, ref(g_friends),
                                 ref(g_friendsLoading));
        } else {
            LoadIncomingRequests(acct.cookie, true);
        }
    }
    SameLine();
    if (Button((string(ICON_USER_PLUS) + " Add Friends").c_str()))
    {
        s_openAddFriendPopup = true;
    }
    EndDisabled();

    if (s_openAddFriendPopup)
    {
        OpenPopup("Add Friends");
        s_openAddFriendPopup = false;
    }
    if (BeginPopupModal("Add Friends", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
    PushTextWrapPos(GetContentRegionAvail().x);
    TextUnformatted("Enter one or more players, separated by commas or new lines. Each entry can be a username or a userId (formatted as id=000).");
    PopTextWrapPos();
    float fieldWidth = GetContentRegionAvail().x;
    float minField = GetFontSize() * 6.25f; // ~100px at 16px
    fieldWidth = (fieldWidth < minField) ? minField : fieldWidth;
    fieldWidth = (fieldWidth < 560.0f) ? 560.0f : fieldWidth; // slightly wider
    string validateErr;
    vector<UserSpecifier> specsPreview;
    string trimmedPreview = trim_copy(s_addFriendBuffer);
    bool hasAny = !trimmedPreview.empty();
    bool validNow = parseMultiUserInput(s_addFriendBuffer, specsPreview, validateErr);
    bool showErr = hasAny && !validNow && !validateErr.empty();
    if (showErr)
        {
            PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
        }
        {
            ImVec2 size(fieldWidth, GetTextLineHeight() * 5.0f + GetStyle().FramePadding.y * 2.0f);
            InputTextMultiline("##AddFriendUser", s_addFriendBuffer, sizeof(s_addFriendBuffer), size, ImGuiInputTextFlags_NoHorizontalScroll);
        }
    if (showErr)
        {
            PopStyleColor();
            PopStyleVar();
        }
        if (s_addFriendLoading.load())
        {
            SameLine();
            TextUnformatted("Sending...");
        }
        Spacing();
        float sendWidth = CalcTextSize("Send").x + GetStyle().FramePadding.x * 2.0f;
        float cancelWidth = CalcTextSize("Cancel").x + GetStyle().FramePadding.x * 2.0f;
    BeginDisabled(!validNow || specsPreview.empty() || s_addFriendLoading.load());
        bool doSend = Button("Send", ImVec2(sendWidth, 0));
        EndDisabled();
        if (doSend)
        {
            string input = trim_copy(s_addFriendBuffer);
            s_addFriendLoading = true;
            vector<UserSpecifier> specs;
            string errTmp;
            (void)parseMultiUserInput(input, specs, errTmp);
            Threading::newThread([specs, cookie = acct.cookie]()
                                 {
                try {
                    int sent = 0;
                    for (const auto &sp : specs) {
                        uint64_t uid = sp.isId ? sp.id : Roblox::getUserIdFromUsername(sp.username);
                        string resp;
                        bool ok = Roblox::sendFriendRequest(to_string(uid), cookie, &resp);
                        if (ok) {
                            ++sent;
                            LOG_INFO("Friend request sent");
                            cerr << "Friend request response: " << resp << "\n";
                        } else {
                            cerr << "Friend request failed: " << resp << "\n";
                            LOG_INFO("Friend request failed");
                        }
                    }
                } catch (const exception &e) {
                    cerr << "Friend request exception: " << e.what() << "\n";
                    LOG_INFO(e.what());
                }
                s_addFriendLoading = false; });
            s_addFriendBuffer[0] = '\0';
            CloseCurrentPopup();
        }
        SameLine(0, GetStyle().ItemSpacing.x);
        if (Button("Cancel", ImVec2(cancelWidth, 0)) && !s_addFriendLoading.load())
        {
            s_addFriendBuffer[0] = '\0';
            CloseCurrentPopup();
        }
        EndPopup();
    }

    // Below: the two-pane content area

    // If Requests view, render requests list with pagination and a details panel.
    if (g_friendsViewMode == 1) {
        float availW = GetContentRegionAvail().x;
        float minSide = GetFontSize() * 14.0f;
        float maxSide = GetFontSize() * 20.0f;
        float listWidth = availW * 0.28f;
        if (listWidth < minSide) listWidth = minSide;
        if (listWidth > maxSide) listWidth = maxSide;

        BeginChild("##RequestsList", ImVec2(listWidth, 0), true);
        // Take a snapshot to render safely without holding the mutex during ImGui calls
        vector<Roblox::IncomingFriendRequest> reqsSnapshot;
        string nextCursorSnapshot;
        {
            std::lock_guard<std::mutex> lk(g_incomingReqMutex);
            reqsSnapshot = g_incomingRequests;
            nextCursorSnapshot = g_incomingReqNextCursor;
        }
        if (g_incomingRequestsLoading.load() && reqsSnapshot.empty()) {
            TextUnformatted("Loading requests...");
        } else {
            for (size_t i = 0; i < reqsSnapshot.size(); ++i) {
                const auto &r = reqsSnapshot[i];
                string header = r.displayName.empty() || r.displayName == r.username
                                ? r.username
                                : r.displayName + " (" + r.username + ")";
                PushID(static_cast<int>(i));
                bool clicked = Selectable(header.c_str(), g_selectedRequestIdx == static_cast<int>(i), ImGuiSelectableFlags_SpanAllColumns);
                // Attach the context menu to the selectable row immediately
                if (BeginPopupContextItem("RequestRowContextMenu")) {
                    if (MenuItem("Copy Display Name")) { SetClipboardText(r.displayName.c_str()); }
                    if (MenuItem("Copy Username")) { SetClipboardText(r.username.c_str()); }
                    if (MenuItem("Copy User ID")) { string idStr = to_string(r.userId); SetClipboardText(idStr.c_str()); }
                    Separator();
                    PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.85f, 0.4f, 1.f));
                    if (MenuItem("Accept Request")) {
                        uint64_t uid = r.userId;
                        string cookieCopy = acct.cookie;
                        Threading::newThread([uid, cookieCopy]() {
                            string resp;
                            bool ok = Roblox::acceptFriendRequest(to_string(uid), cookieCopy, &resp);
                            if (ok) {
                                std::lock_guard<std::mutex> lk(g_incomingReqMutex);
                                erase_if_local(g_incomingRequests, [&](const Roblox::IncomingFriendRequest &x){ return x.userId == uid; });
                                if (g_selectedRequestIdx >= 0 && g_selectedRequestIdx < (int)g_incomingRequests.size()) {
                                    if (g_incomingRequests[g_selectedRequestIdx].userId == uid) g_selectedRequestIdx = -1;
                                } else {
                                    g_selectedRequestIdx = -1;
                                }
                            } else {
                                cerr << "Accept request failed: " << resp << "\n";
                            }
                        });
                    }
                    PopStyleColor();
                    EndPopup();
                }
                // Relative time line under the name
                if (!r.sentAt.empty()) {
                    ImGuiStyle &st = GetStyle();
                    const float indent = st.FramePadding.x * 2.0f;
                    Indent(indent);
                    time_t sent_ts = parseIsoTimestamp(r.sentAt);
                    string relative = sent_ts ? formatRelativeToNow(sent_ts) : string();
                    if (relative.empty()) relative = "just now";
                    TextDisabled("%s", relative.c_str());
                    if (IsItemHovered()) {
                        string absStr = sent_ts ? formatAbsoluteLocal(sent_ts) : r.sentAt;
                        SetTooltip("%s", absStr.c_str());
                    }
                    Unindent(indent);
                }

                if (clicked) {
                    g_selectedRequestIdx = static_cast<int>(i);
                    if (g_selectedRequestDetail.id != r.userId) {
                        g_selectedRequestDetail = {};
                        Threading::newThread(FriendsActions::FetchFriendDetails,
                                             to_string(r.userId),
                                             acct.cookie,
                                             ref(g_selectedRequestDetail),
                                             ref(g_requestDetailsLoading));
                    }
                }

                PopID();
            }
            if (!nextCursorSnapshot.empty()) {
                Spacing();
                BeginDisabled(g_incomingRequestsLoading.load());
                if (Button("Load More")) LoadIncomingRequests(acct.cookie, false);
                EndDisabled();
            }
        }
        EndChild();

        SameLine();

        float desiredTextIndent = 8.0f;
        PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        BeginChild("##RequestDetails", ImVec2(0, 0), true);
        PopStyleVar();
        Indent(desiredTextIndent);
        Spacing();
        // Re-evaluate selection bounds against a current snapshot
        bool selectionValid = false;
        Roblox::IncomingFriendRequest selReq{};
        {
            std::lock_guard<std::mutex> lk(g_incomingReqMutex);
            selectionValid = (g_selectedRequestIdx >= 0 && g_selectedRequestIdx < (int)g_incomingRequests.size());
            if (selectionValid) selReq = g_incomingRequests[g_selectedRequestIdx];
        }
        if (!selectionValid) {
            TextWrapped("Incoming friend requests. Use Refresh to reload, or Load More to paginate.");
        } else if (g_requestDetailsLoading.load()) {
            TextUnformatted("Loading full details...");
        } else {
            const auto &sel = selReq;
            const auto &D = g_selectedRequestDetail;

            ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
            PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 4.0f));
            // Compute label column width based on rows that will actually render right now
            float requestLabelColumnWidth = GetFontSize() * 7.5f;
            {
                vector<const char*> labels;
                labels.push_back("Display Name:");
                labels.push_back("Username:");
                labels.push_back("User ID:");
                labels.push_back("Friends:");
                labels.push_back("Followers:");
                labels.push_back("Following:");
                if (!D.createdIso.empty()) labels.push_back("Created:");
                if (!sel.sentAt.empty()) labels.push_back("Request Sent:");
                if (!sel.originSourceType.empty()) labels.push_back("Request Source:");
                if (sel.sourceUniverseId) labels.push_back("Request Universe ID:");
                labels.push_back("Mutual Friends:");
                labels.push_back("Description:");
                float mx = 0.0f;
                for (const char* lbl : labels) mx = (std::max)(mx, CalcTextSize(lbl).x);
                requestLabelColumnWidth = (std::max)(requestLabelColumnWidth, mx + desiredTextIndent * 2.0f + GetFontSize());
            }
            if (BeginTable("RequestInfoTable", 2, tableFlags)) {
                TableSetupColumn("##reqlabel", ImGuiTableColumnFlags_WidthFixed, requestLabelColumnWidth);
                TableSetupColumn("##reqvalue", ImGuiTableColumnFlags_WidthStretch);

                auto addRow = [&](const char *label, const string &value, bool wrapped=false){
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
                    if (wrapped) TextWrapped("%s", value.c_str()); else TextUnformatted(value.c_str());
                    if (BeginPopupContextItem("CopyReqValue")) { if (MenuItem("Copy")) { SetClipboardText(value.c_str()); } EndPopup(); }
                    PopID();
                    Spacing();
                    Unindent(desiredTextIndent);
                };
                auto addRowInt = [&](const char *label, int v){ addRow(label, to_string(v)); };

                // 1. Display Name
                addRow("Display Name:", !D.displayName.empty() ? D.displayName : (sel.displayName.empty()? sel.username : sel.displayName));
                // 2. Username
                addRow("Username:", !D.username.empty() ? D.username : sel.username);
                // 3. User ID
                addRow("User ID:", to_string(D.id ? D.id : sel.userId));
                // 4. Friends
                if (D.friends) addRowInt("Friends:", D.friends);
                // 5. Followers
                if (D.followers) addRowInt("Followers:", D.followers);
                // 6. Following
                if (D.following) addRowInt("Following:", D.following);
                // 7. Created
                if (!D.createdIso.empty()) addRow("Created:", formatAbsoluteWithRelativeFromIso(D.createdIso));
                // Request-only fields
                if (!sel.sentAt.empty()) addRow("Request Sent:", formatAbsoluteWithRelativeFromIso(sel.sentAt));
                if (!sel.originSourceType.empty()) addRow("Request Source:", sel.originSourceType);
                if (sel.sourceUniverseId) addRow("Request Universe ID:", to_string(sel.sourceUniverseId));

                // Mutual Friends above Description
                TableNextRow();
                TableSetColumnIndex(0);
                Indent(desiredTextIndent);
                Spacing();
                TextUnformatted("Mutual Friends:");
                Spacing();
                Unindent(desiredTextIndent);
                TableSetColumnIndex(1);
                Indent(desiredTextIndent);
                Spacing();
                if (sel.mutuals.empty()) {
                    TextDisabled("No mutual friends");
                } else {
                    for (const auto &m : sel.mutuals) BulletText("%s", m.c_str());
                }
                Spacing();
                Unindent(desiredTextIndent);

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
                float spaceForButtons = GetFrameHeightWithSpacing();
                float reservedHeightBelow = style.ItemSpacing.y + style.ItemSpacing.y + spaceForButtons;
                float descChildHeight = GetContentRegionAvail().y - reservedHeightBelow;
                float minDescHeight = GetTextLineHeightWithSpacing() * 3.0f;
                if (descChildHeight < minDescHeight) descChildHeight = minDescHeight;
                const bool hasDescReq = !D.description.empty();
                const string descStrReq = hasDescReq ? D.description : string("No description");
                PushID("ReqDesc");
                BeginChild("##ReqDescScroll", ImVec2(0, descChildHeight - 4), false, ImGuiWindowFlags_HorizontalScrollbar);
                if (hasDescReq) {
                    TextWrapped("%s", descStrReq.c_str());
                } else {
                    TextDisabled("%s", descStrReq.c_str());
                }
                if (BeginPopupContextItem("CopyReqDesc")) { if (MenuItem("Copy")) { SetClipboardText(descStrReq.c_str()); } EndPopup(); }
                EndChild();
                PopID();

                Spacing();
                Unindent(desiredTextIndent);

                EndTable();
            }
            PopStyleVar();

            Separator();
            // Open Page button (1:1 with Friends)
            Indent(desiredTextIndent / 2);
            bool openProfile = Button((string(ICON_OPEN_LINK) + " Open Page").c_str());
            if (openProfile)
                OpenPopup("ProfileContext");
            OpenPopupOnItemClick("ProfileContext");
            if (BeginPopup("ProfileContext"))
            {
                string primaryCookie;
                string primaryUserId;
                if (!g_selectedAccountIds.empty()) {
                    auto primaryId = *g_selectedAccountIds.begin();
                    auto itp = find_if(g_accounts.begin(), g_accounts.end(),
                        [primaryId](const AccountData &a) { return a.id == primaryId; });
                    if (itp != g_accounts.end()) { primaryCookie = itp->cookie; primaryUserId = itp->userId; }
                }
                const uint64_t uid = D.id ? D.id : sel.userId;
                if (MenuItem("Profile"))
                    if (uid)
                        LaunchWebview("https://www.roblox.com/users/" + to_string(uid) + "/profile",
                                      "Roblox Profile", primaryCookie, primaryUserId);
                if (MenuItem("Friends"))
                    LaunchWebview("https://www.roblox.com/users/" + to_string(uid) + "/friends", "Friends",
                                  primaryCookie, primaryUserId);
                if (MenuItem("Favorites"))
                    LaunchWebview("https://www.roblox.com/users/" + to_string(uid) + "/favorites", "Favorites",
                                  primaryCookie, primaryUserId);
                if (MenuItem("Inventory"))
                    LaunchWebview("https://www.roblox.com/users/" + to_string(uid) + "/inventory/#!/accessories",
                                  "Inventory", primaryCookie, primaryUserId);
                if (MenuItem("Rolimons"))
                    LaunchWebview("https://www.rolimons.com/player/" + to_string(uid), "Rolimons");
                EndPopup();
            }
            Unindent(desiredTextIndent / 2);
        }
        Unindent(desiredTextIndent);
        EndChild();
        return;
    }

    float availW = GetContentRegionAvail().x;
    float minSide = GetFontSize() * 14.0f; // ~224px at 16px
    float maxSide = GetFontSize() * 20.0f; // ~320px at 16px
    float friendsListWidth = availW * 0.28f;
    if (friendsListWidth < minSide) friendsListWidth = minSide;
    if (friendsListWidth > maxSide) friendsListWidth = maxSide;

    BeginChild("##FriendsList", ImVec2(friendsListWidth, 0), true);
    if (g_friendsLoading.load() && g_friends.empty())
    {
        Text("Loading friends...");
    }
    else
    {
        for (size_t i = 0; i < g_friends.size(); ++i)
        {
            const auto &f = g_friends[i];
            PushID(static_cast<int>(i));

            string label;
            const char *icon = presenceIcon(f.presence);
            if (*icon)
                label += icon;
            label += (f.displayName == f.username || f.displayName.empty())
                         ? f.username
                         : (f.displayName + " (" + f.username + ")");

            ImVec4 txtCol = getStatusColor(f.presence);
            PushStyleColor(ImGuiCol_Text, txtCol);

            bool clicked = Selectable(label.c_str(),
                                      g_selectedFriendIdx == static_cast<int>(i),
                                      ImGuiSelectableFlags_SpanAllColumns);
            PopStyleColor();

            if (BeginPopupContextItem("FriendRowContextMenu"))
            {
                if (MenuItem("Copy Display Name"))
                {
                    SetClipboardText(f.displayName.c_str());
                }
                if (MenuItem("Copy Username"))
                {
                    SetClipboardText(f.username.c_str());
                }
                if (MenuItem("Copy User ID"))
                {
                    string idStr = to_string(f.id);
                    SetClipboardText(idStr.c_str());
                }
                bool inGame = f.presence == "InGame" && f.placeId && !f.jobId.empty();
                if (inGame)
                {
                    Separator();
                    StandardJoinMenuParams menu{};
                    menu.placeId = f.placeId;
                    menu.jobId = f.jobId;
                    menu.onLaunchGame = [pid = f.placeId]() {
                        if (g_selectedAccountIds.empty()) return;
                        vector<pair<int, string>> accounts;
                        for (int id : g_selectedAccountIds) {
                            auto itA = find_if(g_accounts.begin(), g_accounts.end(), [&](const AccountData &a) { return a.id == id && AccountFilters::IsAccountUsable(a); });
                            if (itA != g_accounts.end()) accounts.emplace_back(itA->id, itA->cookie);
                        }
                        if (!accounts.empty()) Threading::newThread([pid, accounts]() { launchRobloxSequential(pid, "", accounts); });
                    };
                    menu.onLaunchInstance = [row = f]() {
                        if (g_selectedAccountIds.empty()) return;
                        vector<pair<int, string>> accounts;
                        for (int id : g_selectedAccountIds) {
                            auto itA = find_if(g_accounts.begin(), g_accounts.end(), [&](const AccountData &a) { return a.id == id && AccountFilters::IsAccountUsable(a); });
                            if (itA != g_accounts.end()) accounts.emplace_back(itA->id, itA->cookie);
                        }
                        if (!accounts.empty()) Threading::newThread([row, accounts]() { launchRobloxSequential(row.placeId, row.jobId, accounts); });
                    };
                    menu.onFillGame = [pid = f.placeId]() { FillJoinOptions(pid, ""); };
                    menu.onFillInstance = [row = f]() { FillJoinOptions(row.placeId, row.jobId); };
                    RenderStandardJoinMenu(menu);
                }
                Separator();
                PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
                if (MenuItem("Unfriend"))
                {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "Unfriend %s?", f.username.c_str());
                    FriendInfo fCopy = f;
                    uint64_t friendId = fCopy.id;
                    string cookieCopy = acct.cookie;
                    int acctIdCopy = acct.id;
                    ConfirmPopup::Add(buf, [fCopy, friendId, cookieCopy, acctIdCopy]()
                                      { Threading::newThread([fCopy, friendId, cookieCopy, acctIdCopy]()
                                                             {
                            string resp;
                            bool ok = Roblox::unfriend(to_string(friendId), cookieCopy, &resp);
                            if (ok) {
                                erase_if_local(g_friends, [&](const FriendInfo &fi) {
                                    return fi.id == friendId;
                                });
                                if (g_selectedFriendIdx >= 0 && g_selectedFriendIdx < static_cast<int>
                                    (g_friends.size())
                                    &&
                                    g_friends[g_selectedFriendIdx].id == friendId) {
                                    g_selectedFriendIdx = -1;
                                    g_selectedFriend = {};
                                }
                                erase_if_local(g_accountFriends[acctIdCopy], [&](const FriendInfo &fi) {
                                    return fi.id == friendId;
                                });
                                auto &unfList = g_unfriendedFriends[acctIdCopy];
                                if (std::none_of(unfList.begin(), unfList.end(),
                                                 [&](const FriendInfo &fi) {
                                                     return fi.id == friendId;
                                                 }))
                                    unfList.push_back(fCopy);
                                Data::SaveFriends();
                            } else {
                                cerr << "Unfriend failed: " << resp << "\n";
                            } }); });
                }
                PopStyleColor();
                EndPopup();
            }

            if (f.presence == "InGame" && !f.lastLocation.empty())
            {
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

            if (clicked)
            {
                g_selectedFriendIdx = static_cast<int>(i);
                if (g_selectedFriend.id != f.id)
                {
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

        if (!g_unfriended.empty())
        {
            PushID("FriendsLostSection");
            SeparatorText("Friends Lost");

            if (BeginPopupContextItem("FriendsLostContextMenu"))
            {
                if (MenuItem("Clear"))
                {
                    g_unfriended.clear();
                    g_unfriendedFriends[currentAcctId].clear();
                    Data::SaveFriends();
                }
                EndPopup();
            }
            PopID();

            for (size_t i = 0; i < g_unfriended.size(); ++i)
            {
                const auto &uf = g_unfriended[i];
                string name = uf.displayName.empty() || uf.displayName == uf.username
                                  ? uf.username
                                  : uf.displayName + " (" + uf.username + ")";
                PushID(static_cast<int>(i));
                PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
                TextUnformatted(name.c_str());
                PopStyleColor();
                if (BeginPopupContextItem("FriendsLostEntryMenu"))
                {
                    if (MenuItem("Copy Display Name"))
                    {
                        SetClipboardText(uf.displayName.c_str());
                    }
                    if (MenuItem("Copy Username"))
                    {
                        SetClipboardText(uf.username.c_str());
                    }
                    if (MenuItem("Copy User ID"))
                    {
                        string idStr = to_string(uf.id);
                        SetClipboardText(idStr.c_str());
                    }
                    Separator();
                    PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.85f, 0.4f, 1.f));
                    if (MenuItem("Add Friend"))
                    {
                        uint64_t targetUserId = uf.id;
                        string cookieCopy = acct.cookie;
                        Threading::newThread([targetUserId, cookieCopy]()
                                             {
                            string resp;
                            bool ok = Roblox::sendFriendRequest(to_string(targetUserId), cookieCopy, &resp);
                            if (ok) {
                                LOG_INFO("Friend request sent");
                                cerr << "Friend request response: " << resp << "\n";
                            } else {
                                cerr << "Friend request failed: " << resp << "\n";
                                LOG_INFO("Friend request failed");
                            }
                        });
                    }
                    PopStyleColor();
                    EndPopup();
                }
                PopID();
            }
        }
    }
    EndChild();

    SameLine();

    float desiredTextIndent = 8.0f;
    PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    BeginChild("##FriendDetails", ImVec2(0, 0), true);
    PopStyleVar();

    if (g_selectedFriendIdx < 0 || g_selectedFriendIdx >= static_cast<int>(g_friends.size()))
    {
        Indent(desiredTextIndent);
        Spacing();
        TextWrapped("Click a friend to see more details or take action.");
        Unindent(desiredTextIndent);
    }
    else if (g_friendDetailsLoading.load())
    {
        Indent(desiredTextIndent);
        Spacing();
        Text("Loading full details...");
        Unindent(desiredTextIndent);
    }
    else
    {
        const auto &D = g_selectedFriend;
        if (D.id == 0 && g_friends[g_selectedFriendIdx].id != 0)
        {
            Indent(desiredTextIndent);
            Spacing();
            Text("Fetching details for %s...", g_friends[g_selectedFriendIdx].username.c_str());

            Unindent(desiredTextIndent);
        }
        else if (D.id == 0)
        {
            Indent(desiredTextIndent);
            Spacing();
            TextWrapped("Details not available or selection issue.");
            Unindent(desiredTextIndent);
        }
        else
        {
            ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                         ImGuiTableFlags_SizingFixedFit;
            PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 4.0f));
            float friendLabelColumnWidth = GetFontSize() * 7.5f; // start with a sensible minimum
            {
                // Compute width for this render pass only using rows we will draw
                vector<const char*> labels;
                labels.push_back("Display Name:");
                labels.push_back("Username:");
                labels.push_back("User ID:");
                labels.push_back("Friends:");
                labels.push_back("Followers:");
                labels.push_back("Following:");
                labels.push_back("Created:");
                labels.push_back("Description:");
                float mx = 0.0f;
                for (const char* lbl : labels) mx = (std::max)(mx, CalcTextSize(lbl).x);
                friendLabelColumnWidth = (std::max)(friendLabelColumnWidth, mx + desiredTextIndent * 2.0f + GetFontSize());
            }
            if (BeginTable("FriendInfoTable", 2, tableFlags))
            {
                TableSetupColumn("##friendlabel", ImGuiTableColumnFlags_WidthFixed, friendLabelColumnWidth);
                TableSetupColumn("##friendvalue", ImGuiTableColumnFlags_WidthStretch);

                auto addFriendDataRow = [&](const char *label, const string &value, bool isWrapped = false)
                {
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
                    if (isWrapped)
                    {
                        TextWrapped("%s", value.c_str());
                    }
                    else
                    {
                        TextUnformatted(value.c_str());
                    }
                    if (BeginPopupContextItem("CopyFriendValue"))
                    {
                        if (MenuItem("Copy"))
                        {
                            SetClipboardText(value.c_str());
                        }
                        EndPopup();
                    }
                    PopID();
                    Spacing();
                    Unindent(desiredTextIndent);
                };

                auto addFriendDataRowInt = [&](const char *label, int value)
                {
                    addFriendDataRow(label, to_string(value));
                };

                // 1. Display Name
                addFriendDataRow("Display Name:", D.displayName.empty() ? D.username : D.displayName);
                // 2. Username
                addFriendDataRow("Username:", D.username);
                // 3. User ID
                addFriendDataRow("User ID:", to_string(D.id));
                // 4. Friends
                addFriendDataRowInt("Friends:", D.friends);
                // 5. Followers
                addFriendDataRowInt("Followers:", D.followers);
                // 6. Following
                addFriendDataRowInt("Following:", D.following);
                // 7. Created
                addFriendDataRow("Created:", formatAbsoluteWithRelativeFromIso(D.createdIso));

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
                if (descChildHeight < minDescHeight)
                {
                    descChildHeight = minDescHeight;
                }

                const bool hasDesc = !D.description.empty();
                const string descStr = hasDesc ? D.description : string("No description");
                PushID("FriendDesc");
                BeginChild("##FriendDescScroll", ImVec2(0, descChildHeight - 4), false,
                           ImGuiWindowFlags_HorizontalScrollbar);
                if (hasDesc) {
                    TextWrapped("%s", descStr.c_str());
                } else {
                    TextDisabled("%s", descStr.c_str());
                }
                if (BeginPopupContextItem("CopyFriendDesc"))
                {
                    if (MenuItem("Copy"))
                    {
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

            bool canJoin = (row.presence == "InGame" && row.placeId && !row.jobId.empty());
            BeginDisabled(!canJoin);
            if (Button((string(ICON_JOIN) + " Launch Instance").c_str()) && canJoin)
            {
                vector<pair<int, string>> accounts;
                for (int id : g_selectedAccountIds)
                {
                    auto it = find_if(g_accounts.begin(), g_accounts.end(),
                                      [&](const AccountData &a)
                                      { return a.id == id && AccountFilters::IsAccountUsable(a); });
                    if (it != g_accounts.end())
                        accounts.emplace_back(it->id, it->cookie);
                }
                if (!accounts.empty())
                {
                    Threading::newThread([row, accounts]()
                                         { launchRobloxSequential(row.placeId, row.jobId, accounts); });
                }
            }
            EndDisabled();
            SameLine();
            bool openProfile = Button((string(ICON_OPEN_LINK) + " Open Page").c_str());
            if (openProfile)
                OpenPopup("ProfileContext");
            OpenPopupOnItemClick("ProfileContext");
            if (BeginPopup("ProfileContext"))
            {
                // Use the globally selected primary account for webview auth
                string primaryCookie;
                string primaryUserId;
                if (!g_selectedAccountIds.empty()) {
                    auto primaryId = *g_selectedAccountIds.begin();
                    auto itp = find_if(g_accounts.begin(), g_accounts.end(),
                        [primaryId](const AccountData &a) { return a.id == primaryId; });
                    if (itp != g_accounts.end()) { primaryCookie = itp->cookie; primaryUserId = itp->userId; }
                }
                if (MenuItem("Profile"))
                    if (D.id)
                        LaunchWebview(
                            "https://www.roblox.com/users/" + to_string(D.id) + "/profile",
                            "Roblox Profile", primaryCookie, primaryUserId);
                if (MenuItem("Friends"))
                    LaunchWebview("https://www.roblox.com/users/" + to_string(D.id) + "/friends", "Friends",
                                  primaryCookie, primaryUserId);
                if (MenuItem("Favorites"))
                    LaunchWebview("https://www.roblox.com/users/" + to_string(D.id) + "/favorites", "Favorites",
                                  primaryCookie, primaryUserId);
                if (MenuItem("Inventory"))
                    LaunchWebview("https://www.roblox.com/users/" + to_string(D.id) + "/inventory/#!/accessories",
                                  "Inventory", primaryCookie, primaryUserId);
                if (MenuItem("Rolimons"))
                    LaunchWebview("https://www.rolimons.com/player/" + to_string(D.id), "Rolimons");
                EndPopup();
            }
            Unindent(desiredTextIndent / 2);
        }
    }

    EndChild();
}
