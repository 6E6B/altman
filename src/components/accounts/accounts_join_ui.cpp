#include "accounts_join_ui.h"

#include <imgui.h>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <stdexcept>
#include <utility>

#include "roblox.h"
#include "threading.h"
#include "../data.h"
#include "../../ui.h"
#include "system/launcher.hpp"
#include "core/status.h"
#include "core/logging.hpp"
#include "ui/modal_popup.h"
#include "ui/confirm.h"
#include "core/app_state.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include "system/roblox_control.h"

using namespace ImGui;
using namespace std;

static const char *join_types_local[] = {
    "Place ID",
    "PlaceId + JobId",
    "Username",
};

static const char *GetJoinHintLocal(int idx) {
    switch (idx) {
        case 0:
            return "Enter Place ID...";
        case 2:
            return "Enter Username...";
        default:
            return "";
    }
}


void FillJoinOptions(uint64_t placeId, const std::string &jobId) {
    snprintf(join_value_buf, sizeof(join_value_buf), "%llu", (unsigned long long) placeId);
    if (jobId.empty()) {
        join_jobid_buf[0] = '\0';
        join_type_combo_index = 0;
    } else {
        snprintf(join_jobid_buf, sizeof(join_jobid_buf), "%s", jobId.c_str());
        join_type_combo_index = 1;
    }
    g_activeTab = Tab_Accounts;
}

void RenderJoinOptions() {
    Spacing();
    Text("Join Options");
    Spacing();
    Combo(" Join Type", &join_type_combo_index, join_types_local, IM_ARRAYSIZE(join_types_local));

    if (join_type_combo_index == 1) {
        float w = GetContentRegionAvail().x;
        if (w < 100.0f)
            w = 100.0f;
        PushItemWidth(w);
        InputTextWithHint("##JoinPlaceId", "Enter Place ID...", join_value_buf, IM_ARRAYSIZE(join_value_buf));
        PopItemWidth();
        PushItemWidth(w);
        InputTextWithHint("##JoinJobId", "Enter Job ID...", join_jobid_buf, IM_ARRAYSIZE(join_jobid_buf));
        PopItemWidth();
    } else {
        float w = GetContentRegionAvail().x;
        if (w < 100.0f)
            w = 100.0f;
        PushItemWidth(w);
        InputTextWithHint("##JoinValue",
                          GetJoinHintLocal(join_type_combo_index),
                          join_value_buf,
                          IM_ARRAYSIZE(join_value_buf));
        PopItemWidth();
    }

    Separator();
    if (Button(" \xEF\x8B\xB6  Join ")) {
        auto doJoin = [&]() {
            if (g_selectedAccountIds.empty()) {
                ModalPopup::Add("Select an account first.");
                return;
            }

            if (join_type_combo_index == 2) {
                string username = join_value_buf;
                vector<pair<int, string> > accounts;
                for (int id: g_selectedAccountIds) {
                    auto it = std::find_if(g_accounts.begin(), g_accounts.end(),
                                           [id](auto &a) { return a.id == id; });
                    if (it != g_accounts.end())
                        accounts.emplace_back(it->id, it->cookie);
                }
                if (accounts.empty())
                    return;

                Threading::newThread([username, accounts]() {
                    try {
                        uint64_t uid = Roblox::getUserIdFromUsername(username);
                        auto pres = Roblox::getPresences({uid}, accounts.front().second);
                        auto it = pres.find(uid);
                        if (it == pres.end() || it->second.presence != "InGame" ||
                            it->second.placeId == 0 || it->second.gameId.empty()) {
                            Status::Error("User is not joinable");
                            return;
                        }

                        launchRobloxSequential(it->second.placeId, it->second.gameId, accounts);
                    } catch (const std::exception &e) {
                        LOG_ERROR(std::string("Join by username failed: ") + e.what());
                        Status::Error("Failed to join by username");
                    }
                });
                return;
            }

            uint64_t placeId_val = 0;
            std::string jobId_str;

            try {
                placeId_val = std::stoull(join_value_buf);

                if (join_type_combo_index == 1) {
                    jobId_str = join_jobid_buf;
                } else if (join_type_combo_index != 0) {
                    LOG_ERROR("Error: Join type not supported for direct launch");
                    return;
                }
            } catch (const std::invalid_argument &ia) {
                LOG_ERROR("Invalid numeric input for join: " + std::string(ia.what()));
                return;
            } catch (const std::out_of_range &oor) {
                LOG_ERROR("Numeric input out of range for join: " + std::string(oor.what()));
                return;
            }

            std::vector<std::pair<int, std::string> > accounts;
            for (int id: g_selectedAccountIds) {
                auto it = std::find_if(g_accounts.begin(), g_accounts.end(),
                                       [id](auto &a) { return a.id == id; });
                if (it != g_accounts.end() && it->status != "Banned" && it->status != "Warned" && it->status != "Terminated")
                    accounts.emplace_back(it->id, it->cookie);
            }

            Threading::newThread([placeId_val, jobId_str, accounts]() {
                launchRobloxSequential(placeId_val, jobId_str, accounts);
            });
        };
#ifdef _WIN32
        if (!g_multiRobloxEnabled && RobloxControl::IsRobloxRunning()) {
            ConfirmPopup::Add("Roblox is already running. Launch anyway?", doJoin);
        } else {
            doJoin();
        }
#else
        doJoin();
#endif
    }

    SameLine(0, 10);
    if (Button(" \xEF\x87\xB8  Clear Join Options ")) {
        join_value_buf[0] = '\0';
        join_jobid_buf[0] = '\0';
    }
}
