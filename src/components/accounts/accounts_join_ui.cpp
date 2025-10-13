#include "accounts_join_ui.h"

#include <algorithm>
#include <imgui.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../ui.h"
#include "../../utils/core/account_utils.h"
#include "../data.h"
#include "core/app_state.h"
#include "core/logging.hpp"
#include "core/status.h"
#include "roblox.h"
#include "system/launcher.hpp"
#include "threading.h"
#include "ui/confirm.h"
#include "ui/modal_popup.h"

#ifdef _WIN32
#	include <windows.h>
#endif

#include "system/roblox_control.h"

using namespace ImGui;
using std::all_of;
using std::exception;
using std::find_if;
using std::move;
using std::pair;
using std::string;
using std::to_string;
using std::vector;

static void HelpMarker(const char *desc) {
	TextDisabled("(i)");
	if (IsItemHovered()) {
		BeginTooltip();
		PushTextWrapPos(GetFontSize() * 35.0f);
		TextUnformatted(desc);
		PopTextWrapPos();
		EndTooltip();
	}
}

static const char *join_types_local[] = {
	"Game",
	"Instance",
	"User",
};

static const char *GetJoinHintLocal(int idx) {
	switch (idx) {
	case 0: return "placeId";
	case 2: return "username or userId (id=000)";
	default: return "";
	}
}

void FillJoinOptions(uint64_t placeId, const std::string &jobId) {
	snprintf(join_value_buf, sizeof(join_value_buf), "%llu", (unsigned long long)placeId);
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
	SameLine();
	HelpMarker("Join Options:\n"
			   "- Game: joins a game with its placeId\n"
			   "- Instance: joins the instance of a game with its placeId & jobId\n"
			   "- User: joins the instance a user is in with their username or userId (formatted as id=000)\n"
			   "\t- User option is NOT a sniper, it only works for users who have joins on!");
	Spacing();
	Combo(" Join Type", &join_type_combo_index, join_types_local, IM_ARRAYSIZE(join_types_local));

	if (join_type_combo_index == 1) {
		float w = GetContentRegionAvail().x;
		float minField = GetFontSize() * 6.25f; // ~100px
		float minWide = GetFontSize() * 26.25f; // ~420px
		if (w < minField) { w = minField; }
		if (w < minWide) { w = minWide; }
		PushItemWidth(w);

		bool placeErr = false;
		{
			std::string s = join_value_buf;
			auto l = s.find_first_not_of(" \t\n\r");
			auto r = s.find_last_not_of(" \t\n\r");
			if (l == std::string::npos) {
				s.clear();
			} else {
				s = s.substr(l, r - l + 1);
			}
			if (!s.empty()) {
				for (char c : s) {
					if (c < '0' || c > '9') {
						placeErr = true;
						break;
					}
				}
			}
		}
		if (placeErr) {
			PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
			PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
		}
		InputTextWithHint("##JoinPlaceId", "placeId", join_value_buf, IM_ARRAYSIZE(join_value_buf));
		if (placeErr) {
			PopStyleColor();
			PopStyleVar();
		}
		PopItemWidth();
		PushItemWidth(w);
		bool jobErr = false;
		{
			std::string s = join_jobid_buf;
			auto l = s.find_first_not_of(" \t\n\r");
			auto r = s.find_last_not_of(" \t\n\r");
			if (l == std::string::npos) {
				s.clear();
			} else {
				s = s.substr(l, r - l + 1);
			}
			if (!s.empty()) {
				auto isHex
					= [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
				const int parts[5] = {8, 4, 4, 4, 12};
				int idx = 0;
				size_t pos = 0;
				for (int p = 0; p < 5; ++p) {
					for (int k = 0; k < parts[p]; ++k) {
						if (pos >= s.size() || !isHex(s[pos++])) {
							jobErr = true;
							break;
						}
					}
					if (jobErr) { break; }
					if (p < 4) {
						if (pos >= s.size() || s[pos++] != '-') {
							jobErr = true;
							break;
						}
					}
				}
				if (!jobErr && pos != s.size()) { jobErr = true; }
			}
		}
		if (jobErr) {
			PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
			PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
		}
		InputTextWithHint("##JoinJobId", "jobId", join_jobid_buf, IM_ARRAYSIZE(join_jobid_buf));
		if (jobErr) {
			PopStyleColor();
			PopStyleVar();
		}
		PopItemWidth();
	} else {
		float w = GetContentRegionAvail().x;
		float minField = GetFontSize() * 6.25f; // ~100px
		float minWide = GetFontSize() * 26.25f; // ~420px
		if (w < minField) { w = minField; }
		if (w < minWide) { w = minWide; }
		PushItemWidth(w);
		bool showError = false;
		if (join_type_combo_index == 2) {
			std::string preview = join_value_buf;
			UserSpecifier tmp {};
			std::string trimmed = preview;
			auto l = trimmed.find_first_not_of(" \t\n\r");
			auto r = trimmed.find_last_not_of(" \t\n\r");
			if (l == std::string::npos) {
				trimmed.clear();
			} else {
				trimmed = trimmed.substr(l, r - l + 1);
			}
			if (!trimmed.empty() && !parseUserSpecifier(trimmed, tmp)) { showError = true; }
		} else if (join_type_combo_index == 0) {
			std::string s = join_value_buf;
			auto l = s.find_first_not_of(" \t\n\r");
			auto r = s.find_last_not_of(" \t\n\r");
			if (l == std::string::npos) {
				s.clear();
			} else {
				s = s.substr(l, r - l + 1);
			}
			if (!s.empty()) {
				for (char c : s) {
					if (c < '0' || c > '9') {
						showError = true;
						break;
					}
				}
			}
		}
		if (showError) {
			PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
			PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
		}
		InputTextWithHint(
			"##JoinValue",
			GetJoinHintLocal(join_type_combo_index),
			join_value_buf,
			IM_ARRAYSIZE(join_value_buf)
		);
		if (showError) {
			PopStyleColor();
			PopStyleVar();
		}
		PopItemWidth();
	}

	Separator();
	bool allowJoin = true;
	if (join_type_combo_index == 2) {
		std::string s = join_value_buf;
		auto l = s.find_first_not_of(" \t\n\r");
		auto r = s.find_last_not_of(" \t\n\r");
		if (l == std::string::npos) {
			s.clear();
		} else {
			s = s.substr(l, r - l + 1);
		}
		UserSpecifier tmp {};
		if (s.empty() || !parseUserSpecifier(s, tmp)) { allowJoin = false; }
	} else if (join_type_combo_index == 1) {
		std::string pid = join_value_buf;
		std::string jid = join_jobid_buf;
		auto trim = [](std::string &x) {
			auto l = x.find_first_not_of(" \t\n\r");
			auto r = x.find_last_not_of(" \t\n\r");
			if (l == std::string::npos) {
				x.clear();
			} else {
				x = x.substr(l, r - l + 1);
			}
		};
		trim(pid);
		trim(jid);
		if (pid.empty() || !std::all_of(pid.begin(), pid.end(), [](char c) { return c >= '0' && c <= '9'; })) {
			allowJoin = false;
		}
		auto isHex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
		if (!jid.empty()) {
			const int parts[5] = {8, 4, 4, 4, 12};
			size_t pos = 0;
			bool err = false;
			for (int p = 0; p < 5; ++p) {
				for (int k = 0; k < parts[p]; ++k) {
					if (pos >= jid.size() || !isHex(jid[pos++])) {
						err = true;
						break;
					}
				}
				if (err) { break; }
				if (p < 4) {
					if (pos >= jid.size() || jid[pos++] != '-') {
						err = true;
						break;
					}
				}
			}
			if (err || pos != jid.size()) { allowJoin = false; }
		} else {
			allowJoin = false;
		}
	} else if (join_type_combo_index == 0) {
		std::string pid = join_value_buf;
		auto l = pid.find_first_not_of(" \t\n\r");
		auto r = pid.find_last_not_of(" \t\n\r");
		if (l == std::string::npos) {
			pid.clear();
		} else {
			pid = pid.substr(l, r - l + 1);
		}
		if (pid.empty() || !std::all_of(pid.begin(), pid.end(), [](char c) { return c >= '0' && c <= '9'; })) {
			allowJoin = false;
		}
	}
	BeginDisabled(!allowJoin);
	if (Button(" \xEF\x8B\xB6  Launch ")) {
		auto doJoin = [&]() {
			if (g_selectedAccountIds.empty()) {
				ModalPopup::Add("Select an account first.");
				return;
			}

			if (join_type_combo_index == 2) {
				string userInput = join_value_buf;
				vector<pair<int, string>> accounts;
				for (int id : g_selectedAccountIds) {
					auto it = std::find_if(g_accounts.begin(), g_accounts.end(), [id](auto &a) { return a.id == id; });
					if (it != g_accounts.end() && AccountFilters::IsAccountUsable(*it)) {
						accounts.emplace_back(it->id, it->cookie);
					}
				}
				if (accounts.empty()) { return; }

				Threading::newThread([userInput, accounts]() {
					try {
						UserSpecifier spec {};
						if (!parseUserSpecifier(userInput, spec)) {
							Status::Error("Enter username or userId (id=000)");
							return;
						}
						uint64_t uid = 0;
						if (spec.isId) {
							uid = spec.id;
						} else {
							uid = Roblox::getUserIdFromUsername(spec.username);
						}
						auto pres = Roblox::getPresences({uid}, accounts.front().second);
						auto it = pres.find(uid);
						if (it == pres.end() || it->second.presence != "InGame" || it->second.placeId == 0
							|| it->second.jobId.empty()) {
							Status::Error("User is not joinable");
							return;
						}

						launchRobloxSequential(it->second.placeId, it->second.jobId, accounts);
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

			std::vector<std::pair<int, std::string>> accounts;
			for (int id : g_selectedAccountIds) {
				auto it = std::find_if(g_accounts.begin(), g_accounts.end(), [id](auto &a) { return a.id == id; });
				if (it != g_accounts.end() && AccountFilters::IsAccountUsable(*it)) {
					accounts.emplace_back(it->id, it->cookie);
				}
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
	EndDisabled();

	SameLine(0, 10);
	if (Button(" \xEF\x87\xB8  Clear Join Options ")) {
		join_value_buf[0] = '\0';
		join_jobid_buf[0] = '\0';
		join_type_combo_index = 0;
	}
}
