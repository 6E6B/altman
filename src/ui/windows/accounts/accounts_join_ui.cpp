#include "accounts_join_ui.h"

#include <imgui.h>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <stdexcept>
#include <utility>
#include <ranges>
#include <string_view>
#include <format>
#include <cctype>
#include <array>
#include <sstream>

#include "ui/ui.h"
#include "utils/account_utils.h"
#include "components/data.h"
#include "console/console.h"
#include "components/data.h"
#include "ui/widgets/bottom_right_status.h"
#include "network/roblox/common.h"
#include "network/roblox/auth.h"
#include "network/roblox/games.h"
#include "network/roblox/session.h"
#include "network/roblox/social.h"
#include "system/roblox_control.h"
#include "system/roblox_launcher.h"
#include "utils/thread_task.h"
#include "ui/widgets/modal_popup.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
    constexpr std::string_view ICON_LAUNCH = "\xEF\x8B\xB6";
    constexpr std::string_view ICON_CLEAR = "\xEF\x87\xB8";
    constexpr float MIN_FIELD_WIDTH = 100.0f;  // ~100px at 16px font
    constexpr float MIN_WIDE_WIDTH = 420.0f;   // ~420px

    constexpr std::array<const char*, 4> JOIN_TYPE_NAMES = {
        "Private Server",
    	"Game",
        "Game Server",
        "User"
    };

    // UUID validation parts for jobId
    constexpr std::array<int, 5> UUID_PARTS = {8, 4, 4, 4, 12};

    std::string trim(std::string_view sv) {
        const auto start = std::find_if_not(sv.begin(), sv.end(), 
            [](char c) { return std::isspace(static_cast<unsigned char>(c)); });
        
        const auto end = std::find_if_not(sv.rbegin(), (std::string_view::reverse_iterator)start, 
            [](char c) { return std::isspace(static_cast<unsigned char>(c)); }).base();

        if (start >= end) {
            return "";
        }
        return std::string(start, end);
    }

    bool isHexadecimalString(std::string_view sv) noexcept {
        return !sv.empty() && std::ranges::all_of(sv, [](char c){ return std::isxdigit(static_cast<unsigned char>(c)); });
    }

    bool isNumericString(std::string_view sv) noexcept {
        return !sv.empty() && std::ranges::all_of(sv, [](char c){ return std::isdigit(static_cast<unsigned char>(c)); });
    }

    bool validateUUID(std::string_view uuid) {
        if (uuid.empty()) return false;
        
        std::size_t pos = 0;
        for (int part = 0; part < 5; ++part) {
            // Check hex digits
            for (int i = 0; i < UUID_PARTS[part]; ++i) {
                if (pos >= uuid.size() || !std::isxdigit(static_cast<unsigned char>(uuid[pos++]))) {
                    return false;
                }
            }
            // Check hyphen separator (except after last part)
            if (part < 4) {
                if (pos >= uuid.size() || uuid[pos++] != '-') {
                    return false;
                }
            }
        }
        return pos == uuid.size();
    }

    constexpr const char* getJoinHint(JoinType joinType) noexcept {
        switch (joinType) {
			case JoinType::PrivateServer: return "private server link (e.g., https://www.roblox.com/share?code=...)";
            case JoinType::Game: return "placeId (e.g., 606849621)";
            case JoinType::User: return "username or userId (id=000)";
            default: return "";
        }
    }

    void renderHelpMarker(const char* desc) {
        ImGui::TextDisabled("(i)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::TextUnformatted(desc);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    float calculateInputWidth() {
        float width = ImGui::GetContentRegionAvail().x;
        width = std::max(width, MIN_FIELD_WIDTH);
        width = std::max(width, MIN_WIDE_WIDTH);
        return width;
    }

    void renderErrorBorder() {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
    }

    void endErrorBorder() {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    struct ValidationResult {
        bool isValid{false};
        bool showError{false};
    };

    ValidationResult validatePlaceId(std::string_view input) {
        const auto trimmed = trim(input);
        if (trimmed.empty()) {
            return {false, false};
        }
        const bool valid = isNumericString(trimmed);
        return {valid, !valid};
    }

    ValidationResult validateJobId(std::string_view input) {
        const auto trimmed = trim(input);
        if (trimmed.empty()) {
            return {false, true};
        }
        const bool valid = validateUUID(trimmed);
        return {valid, !valid};
    }

    ValidationResult validateUserInput(std::string_view input) {
        const auto trimmed = trim(input);
        if (trimmed.empty()) {
            return {false, false};
        }
        UserSpecifier tmp{};
        const bool valid = parseUserSpecifier(std::string(trimmed), tmp);
        return {valid, !valid};
    }

    ValidationResult validatePrivateServerLink(std::string_view link) {
        std::string trimmed = trim(link);  
        if (trimmed.empty()) {
            return {false, false};
        }

        std::string_view s = trimmed;

        if (s.size() < 15 || !(s.starts_with("https://")) || s.find("roblox.com") == std::string_view::npos)
        {
            return {false, true};
        }

        constexpr std::string_view code_key = "code=";
        if (auto pos = s.find(code_key); pos != std::string_view::npos)
        {
            std::string_view code = s.substr(pos + code_key.size());
            if (auto amp = code.find('&'); amp != std::string_view::npos)
                code = code.substr(0, amp);

            bool valid = !code.empty() &&
                        code.size() >= 16 &&
                        isHexadecimalString(code);

            return {valid, !valid};
        }

        constexpr std::string_view link_key = "privateServerLinkCode=";
        if (auto pos = s.find(link_key); pos != std::string_view::npos)
        {
            std::string_view code = s.substr(pos + link_key.size());
            if (auto amp = code.find('&'); amp != std::string_view::npos)
                code = code.substr(0, amp);

            bool valid = !code.empty() && isNumericString(code);
            return {valid, !valid};
        }

        return {false, true};
    }


	void handleJoinByUser(std::string userInput) {
    	auto accountPtrs = getUsableSelectedAccounts();
    	if (accountPtrs.empty()) {
    		ButtonRightStatus::Error("No usable accounts selected");
    		return;
    	}

    	// Copy for thread safety
    	std::vector<AccountData> accounts;
    	accounts.reserve(accountPtrs.size());
    	for (AccountData* acc : accountPtrs) {
    		accounts.push_back(*acc);
    	}

    	ThreadTask::fireAndForget([userInput = std::move(userInput),
								   accounts = std::move(accounts)]() {
			try {
				UserSpecifier spec{};
				if (!parseUserSpecifier(userInput, spec)) {
					ButtonRightStatus::Error("Enter username or userId (id=000)");
					return;
				}

				const uint64_t uid = spec.isId ? spec.id : Roblox::getUserIdFromUsername(spec.username);
				const auto presenceMap = Roblox::getPresences({uid}, accounts.front().cookie);

				const auto it = presenceMap.find(uid);
				if (it == presenceMap.end() || it->second.presence != "InGame" ||
					it->second.placeId == 0 || it->second.jobId.empty()) {
					ButtonRightStatus::Error("User is not joinable");
					return;
				}

				launchWithAccounts(LaunchParams::gameJob(it->second.placeId, it->second.jobId), accounts);
			} catch (const std::exception& e) {
				LOG_ERROR("Join by username failed: {}", e.what());
				ButtonRightStatus::Error("Failed to join by username");
			}
		});
    }

    void handleJoinByPrivateServer(std::string serverLink) {
    	launchWithSelectedAccounts(LaunchParams::privateServer(serverLink));
    }

    void handleJoinByPlaceId(uint64_t placeId, std::string jobId) {
    	launchWithSelectedAccounts(LaunchParams::gameJob(placeId, jobId));
    }

    void renderInstanceInputs() {
        const float width = calculateInputWidth();
        
        // Place ID input
        ImGui::PushItemWidth(width);
        const auto placeValidation = validatePlaceId(join_value_buf);
        if (placeValidation.showError) renderErrorBorder();
        ImGui::InputTextWithHint("##JoinPlaceId", "placeId", join_value_buf, IM_ARRAYSIZE(join_value_buf));
        if (placeValidation.showError) endErrorBorder();
        ImGui::PopItemWidth();

        // Job ID input
        ImGui::PushItemWidth(width);
        const auto jobValidation = validateJobId(join_jobid_buf);
        if (jobValidation.showError) renderErrorBorder();
        ImGui::InputTextWithHint("##JoinJobId", "jobId", join_jobid_buf, IM_ARRAYSIZE(join_jobid_buf));
        if (jobValidation.showError) endErrorBorder();
        ImGui::PopItemWidth();
    }

    void renderSingleInput(int joinType) {
        const float width = calculateInputWidth();
        ImGui::PushItemWidth(width);

        bool showError = false;
    	if (joinType == JoinType::PrivateServer) {
    		showError = validatePrivateServerLink(join_value_buf).showError;
        } else if (joinType == JoinType::Game) {
            showError = validatePlaceId(join_value_buf).showError;
        } else if (joinType == JoinType::GameServer) {
        	//showError = validateJobId(join_value_buf).showError;
        } else if (joinType == JoinType::User) {
	        showError = validateUserInput(join_value_buf).showError;
        }

        if (showError) renderErrorBorder();
        ImGui::InputTextWithHint("##JoinValue", getJoinHint(static_cast<JoinType>(joinType)), 
                                join_value_buf, IM_ARRAYSIZE(join_value_buf));
        if (showError) endErrorBorder();
        ImGui::PopItemWidth();
    }

    bool canJoin(int joinType) {
        switch (joinType) {
	        case JoinType::PrivateServer:
        		return validatePrivateServerLink(join_value_buf).isValid;
	        case JoinType::Game: {
        		const auto placeId = trim(join_value_buf);
        		return !placeId.empty() && isNumericString(placeId);
	        }
            case JoinType::GameServer: {
                const auto placeId = trim(join_value_buf);
                const auto jobId = trim(join_jobid_buf);
                return !placeId.empty() && isNumericString(placeId) && validateUUID(jobId);
            }
	        case JoinType::User: {
            		const auto trimmed = trim(join_value_buf);
            		if (trimmed.empty()) return false;
            		UserSpecifier tmp{};
            		return parseUserSpecifier(trimmed, tmp);
	        }
            default:
                return false;
        }
    }

    void performJoin() {
        if (g_selectedAccountIds.empty()) {
            ModalPopup::AddInfo("Select an account first.");
            return;
        }

        const auto joinType = static_cast<JoinType>(join_type_combo_index);

        switch (joinType) {
	        case JoinType::PrivateServer:
        		handleJoinByPrivateServer(join_value_buf);
        		break;
            case JoinType::Game:
            case JoinType::GameServer: {
                try {
                    std::stringstream ss(join_value_buf);
                    uint64_t placeId;
                    if (!(ss >> placeId)) {
                        throw std::invalid_argument("Failed to parse Place ID");
                    }

                    const std::string jobId = (joinType == JoinType::GameServer)
                        ? join_jobid_buf : std::string();
                        
                    handleJoinByPlaceId(placeId, jobId);
                } catch (const std::invalid_argument& e) {
                    LOG_ERROR("Invalid numeric input: {}", e.what());
                } catch (const std::out_of_range& e) {
                    LOG_ERROR("Numeric input out of range: {}", e.what());
                }
                break;
            }
	        case JoinType::User:
        		handleJoinByUser(join_value_buf);
        		break;
            default:
                LOG_ERROR("Unsupported join type");
                break;
        }
    }

} 

void FillJoinOptions(uint64_t placeId, const std::string& jobId) {
    std::snprintf(join_value_buf, sizeof(join_value_buf), "%llu",
                  static_cast<unsigned long long>(placeId));
    
    if (jobId.empty()) {
        join_jobid_buf[0] = '\0';
        join_type_combo_index = JoinType::Game;
    } else {
        std::snprintf(join_jobid_buf, sizeof(join_jobid_buf), "%s", jobId.c_str());
        join_type_combo_index = JoinType::GameServer;
    }
    g_activeTab = Tab_Accounts;
}

void RenderJoinOptions() {
    ImGui::Spacing();
    ImGui::Text("Join Options");
    ImGui::SameLine();
    renderHelpMarker(
        "Join Options:\n"
        "- Game: joins a game with its placeId\n"
        "- GameServer: joins the instance of a game with its placeId & jobId\n"
        "- User: joins the instance a user is in with their username or userId (formatted as id=000)\n"
        "\t- User option is NOT a sniper, it only works for users who have joins on!\n"
        "- Private server: joins private server by share link\n"
    );
    ImGui::Spacing();

    ImGui::Combo(" Join Type", &join_type_combo_index,
                JOIN_TYPE_NAMES.data(), static_cast<int>(JOIN_TYPE_NAMES.size()));

    // Render appropriate inputs based on join type
    if (join_type_combo_index == JoinType::GameServer) {
        renderInstanceInputs();
    } else {
        renderSingleInput(join_type_combo_index);
    }

    ImGui::Separator();

    // Launch button
    const bool allowJoin = canJoin(join_type_combo_index);
    ImGui::BeginDisabled(!allowJoin);

    if (ImGui::Button(std::format(" {}  Launch ", ICON_LAUNCH).c_str())) {
        auto doJoin = [&]() { performJoin(); };
        doJoin();
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0, 10);
    if (ImGui::Button(std::format(" {}  Clear Join Options ", ICON_CLEAR).c_str())) {
        join_value_buf[0] = '\0';
        join_jobid_buf[0] = '\0';
    }
}