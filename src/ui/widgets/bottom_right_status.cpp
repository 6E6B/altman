#include "bottom_right_status.h"

#include <mutex>
#include <cmath>

namespace ButtonRightStatus {
    namespace {
        constexpr const char* ICON_MOON = "\xEF\x86\x86";
        constexpr const char* ICON_CHECK = "\xEF\x80\x8C";
        constexpr const char* ICON_SPINNER = "\xEF\x84\x90";
        constexpr const char* ICON_TRIANGLE = "\xEF\x81\xB1";
        constexpr const char* ICON_TIMES = "\xEF\x80\x8D";
        constexpr const char* ICON_USER = "\xEF\x80\x87";

        constexpr float ANIMATION_DURATION = 2.0f;
        constexpr float DOT_ANIMATION_SPEED = 2.5f;
        constexpr float SPINNER_SPEED = 4.0f;
        constexpr float DEFAULT_DURATION = 8.0f;

        struct State {
            std::string currentText;
            std::string previousText;
            Type currentType = Type::Idle;
            Type previousType = Type::Idle;

            float duration = 0.0f;
            float elapsed = 0.0f;

            float transitionProgress = 1.0f;
            bool isTransitioning = false;
        };

        std::mutex g_mutex;
        State g_state;

        const char* GetIcon(Type type, bool hasSelectedAccounts) {
            switch (type) {
                case Type::Idle:    return hasSelectedAccounts ? ICON_USER : ICON_MOON;
                case Type::Loading: return ICON_SPINNER;
                case Type::Success: return ICON_CHECK;
                case Type::Warning: return ICON_TRIANGLE;
                case Type::Error:   return ICON_TIMES;
                default:            return ICON_MOON;
            }
        }

        ImVec4 GetIconColor(Type type, bool hasSelectedAccounts) {
            switch (type) {
                case Type::Idle:
                    return hasSelectedAccounts
                        ? ImVec4(0.5f, 0.7f, 0.9f, 1.0f)
                        : ImVec4(0.5f, 0.5f, 0.6f, 1.0f);
                case Type::Loading: return ImVec4(0.4f, 0.6f, 0.9f, 1.0f);
                case Type::Success: return ImVec4(0.3f, 0.9f, 0.4f, 1.0f);
                case Type::Warning: return ImVec4(0.9f, 0.7f, 0.2f, 1.0f);
                case Type::Error:   return ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
                default:            return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            }
        }

        ImVec4 GetTextColor(Type type) {
            switch (type) {
                case Type::Idle:    return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
                case Type::Loading: return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
                case Type::Success: return ImVec4(0.8f, 1.0f, 0.8f, 1.0f);
                case Type::Warning: return ImVec4(1.0f, 0.9f, 0.7f, 1.0f);
                case Type::Error:   return ImVec4(1.0f, 0.8f, 0.8f, 1.0f);
                default:            return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            }
        }

        [[maybe_unused]]
        std::string GetAnimatedDots(float time) {
            const int dotCount = static_cast<int>(time * DOT_ANIMATION_SPEED) % 4;
            switch (dotCount) {
                case 0: return "";
                case 1: return ".";
                case 2: return "..";
                case 3: return "...";
                default: return "";
            }
        }

        float EaseOutCubic(float t) {
            return 1.0f - std::pow(1.0f - t, 3.0f);
        }

        void AddTextRotated(ImDrawList* drawList, const ImVec2& pos, ImU32 col, const char* text, float angle) {
            const ImVec2 textSize = ImGui::CalcTextSize(text);
            const ImVec2 center(pos.x + textSize.x * 0.5f, pos.y + textSize.y * 0.5f);

            const int vtxStart = drawList->VtxBuffer.Size;

            drawList->AddText(pos, col, text);

            const int vtxEnd = drawList->VtxBuffer.Size;

            const float cosA = std::cos(angle);
            const float sinA = std::sin(angle);

            for (int i = vtxStart; i < vtxEnd; ++i) {
                ImVec2& vtx = drawList->VtxBuffer[i].pos;
                const float dx = vtx.x - center.x;
                const float dy = vtx.y - center.y;
                vtx.x = center.x + dx * cosA - dy * sinA;
                vtx.y = center.y + dx * sinA + dy * cosA;
            }
        }
    }

    void AddText(const std::string& text, Type type, float duration) {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (text == g_state.currentText && type == g_state.currentType) {
            g_state.elapsed = 0.0f;
            return;
        }

        g_state.previousText = g_state.currentText;
        g_state.previousType = g_state.currentType;
        g_state.currentText = text;
        g_state.currentType = type;
        g_state.duration = duration;
        g_state.elapsed = 0.0f;
        g_state.transitionProgress = 0.0f;
        g_state.isTransitioning = true;
    }

    void Loading(const std::string& text) {
        AddText(text, Type::Loading, 0.0f);
    }

    void Success(const std::string& text, float duration) {
        AddText(text, Type::Success, duration);
    }

    void Info(const std::string& text, float duration) {
        AddText(text, Type::Idle, duration);
    }

	void Warning(const std::string& text, float duration) {
    	AddText(text, Type::Warning, duration);
    }

	void Error(const std::string& text, float duration) {
    	AddText(text, Type::Error, duration);
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (g_state.currentType == Type::Idle && g_state.currentText.empty()) {
            return;
        }

        g_state.previousText = g_state.currentText;
        g_state.previousType = g_state.currentType;
        g_state.currentText = "";
        g_state.currentType = Type::Idle;
        g_state.duration = 0.0f;
        g_state.elapsed = 0.0f;
        g_state.transitionProgress = 0.0f;
        g_state.isTransitioning = true;
    }

    void Render(const ImGuiViewport* viewport, float deltaTime,
                const std::vector<SelectedAccount>& selectedAccounts) {
        std::lock_guard<std::mutex> lock(g_mutex);

        const bool hasSelectedAccounts = !selectedAccounts.empty();

        if (g_state.isTransitioning) {
            g_state.transitionProgress += deltaTime / ANIMATION_DURATION;
            if (g_state.transitionProgress >= 1.0f) {
                g_state.transitionProgress = 1.0f;
                g_state.isTransitioning = false;
            }
        }

        g_state.elapsed += deltaTime;
        if (g_state.duration > 0.0f && g_state.elapsed >= g_state.duration) {
            if (g_state.currentType != Type::Idle || !g_state.currentText.empty()) {
                g_state.previousText = g_state.currentText;
                g_state.previousType = g_state.currentType;
                g_state.currentText = "";
                g_state.currentType = Type::Idle;
                g_state.duration = 0.0f;
                g_state.elapsed = 0.0f;
                g_state.transitionProgress = 0.0f;
                g_state.isTransitioning = true;
            }
        }

        const ImVec2 position(
            viewport->WorkPos.x + viewport->WorkSize.x,
            viewport->WorkPos.y + viewport->WorkSize.y
        );

        ImGui::SetNextWindowPos(position, ImGuiCond_Always, ImVec2(1, 1));

        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoFocusOnAppearing;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

        if (ImGui::Begin("##StatusBar", nullptr, flags)) {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const float time = static_cast<float>(ImGui::GetTime());
            const float lineHeight = ImGui::GetTextLineHeight();

            const bool isIdle = g_state.currentType == Type::Idle && g_state.currentText.empty();
            const bool wasIdle = g_state.previousType == Type::Idle && g_state.previousText.empty();

            const char* currentIcon = GetIcon(g_state.currentType, hasSelectedAccounts && isIdle);
            const char* previousIcon = GetIcon(g_state.previousType, hasSelectedAccounts && wasIdle);

            ImVec4 currentIconColor = GetIconColor(g_state.currentType, hasSelectedAccounts && isIdle);
            ImVec4 previousIconColor = GetIconColor(g_state.previousType, hasSelectedAccounts && wasIdle);

            // Icon breathing animation (only when not transitioning and not loading)
            if (!g_state.isTransitioning && g_state.currentType != Type::Loading) {
                float iconAlpha = 1.0f;
                if (isIdle) {
                    iconAlpha = 0.6f + 0.2f * std::sin(time * 1.5f);
                }
                currentIconColor.w *= iconAlpha;
            }

            const ImVec2 iconStartPos = ImGui::GetCursorScreenPos();
            const float iconWidth = ImGui::CalcTextSize(currentIcon).x;

            // Draw icon with roulette animation
            const ImVec2 iconClipMin(iconStartPos.x - 2.0f, iconStartPos.y);
            const ImVec2 iconClipMax(iconStartPos.x + iconWidth + 4.0f, iconStartPos.y + lineHeight);

            drawList->PushClipRect(iconClipMin, iconClipMax, true);

            if (g_state.isTransitioning) {
                const float easedProgress = EaseOutCubic(g_state.transitionProgress);

                // Previous icon scrolls up
                const float prevOffset = -easedProgress * lineHeight;
                previousIconColor.w = 1.0f - easedProgress;

                const ImVec2 prevIconPos(iconStartPos.x, iconStartPos.y + prevOffset);

                // Spin previous icon if it was loading
                if (g_state.previousType == Type::Loading) {
                    AddTextRotated(drawList, prevIconPos, ImGui::ColorConvertFloat4ToU32(previousIconColor),
                                   previousIcon, time * SPINNER_SPEED);
                } else {
                    drawList->AddText(prevIconPos, ImGui::ColorConvertFloat4ToU32(previousIconColor), previousIcon);
                }

                // Current icon scrolls up from below
                const float currOffset = (1.0f - easedProgress) * lineHeight;
                currentIconColor.w = easedProgress;

                const ImVec2 currIconPos(iconStartPos.x, iconStartPos.y + currOffset);

                // Spin current icon if loading
                if (g_state.currentType == Type::Loading) {
                    AddTextRotated(drawList, currIconPos, ImGui::ColorConvertFloat4ToU32(currentIconColor),
                                   currentIcon, time * SPINNER_SPEED);
                } else {
                    drawList->AddText(currIconPos, ImGui::ColorConvertFloat4ToU32(currentIconColor), currentIcon);
                }
            } else {
                // Static or spinning display
                if (g_state.currentType == Type::Loading) {
                    AddTextRotated(drawList, iconStartPos, ImGui::ColorConvertFloat4ToU32(currentIconColor),
                                   currentIcon, time * SPINNER_SPEED);
                } else {
                    drawList->AddText(iconStartPos, ImGui::ColorConvertFloat4ToU32(currentIconColor), currentIcon);
                }
            }

            drawList->PopClipRect();

            // Reserve space for icon
            ImGui::Dummy(ImVec2(iconWidth, lineHeight));

            ImGui::SameLine();
            ImGui::Spacing();
            ImGui::SameLine();

            if (isIdle && hasSelectedAccounts && !g_state.isTransitioning) {
                ImGui::TextUnformatted("Selected: ");
                ImGui::SameLine(0, 0);

                for (std::size_t i = 0; i < selectedAccounts.size(); ++i) {
                    if (i > 0) {
                        ImGui::TextUnformatted(", ");
                        ImGui::SameLine(0, 0);
                    }

                    ImGui::PushStyleColor(ImGuiCol_Text, selectedAccounts[i].color);
                    ImGui::TextUnformatted(selectedAccounts[i].label.c_str());
                    ImGui::PopStyleColor();

                    if (i == 0 && selectedAccounts.size() > 1) {
                        ImGui::SameLine(0, 0);
                        ImGui::TextUnformatted("*");
                    }

                    if (i + 1 < selectedAccounts.size()) {
                        ImGui::SameLine(0, 0);
                    }
                }
            }
            else if (isIdle && !g_state.isTransitioning) {
                ImVec4 textColor = GetTextColor(Type::Idle);
                ImGui::PushStyleColor(ImGuiCol_Text, textColor);
                ImGui::TextUnformatted("Ready");
                ImGui::PopStyleColor();
            }
            // Show status text with roulette animation
            else {
                const ImVec2 textStartPos = ImGui::GetCursorScreenPos();

                std::string currentDisplay = g_state.currentText;
            	/*if (g_state.currentType == Type::Loading) {
            		currentDisplay += GetAnimatedDots(time);
            	}*/

                if (g_state.duration > 0.0f) {
                    const int remaining = static_cast<int>(std::ceil(g_state.duration - g_state.elapsed));
                    if (remaining > 0) {
                        currentDisplay += " (" + std::to_string(remaining) + ")";
                    }
                }

                if (isIdle) {
                    if (hasSelectedAccounts) {
                        currentDisplay = "Selected: ";
                        for (std::size_t i = 0; i < selectedAccounts.size(); ++i) {
                            if (i > 0) currentDisplay += ", ";
                            currentDisplay += selectedAccounts[i].label;
                            if (i == 0 && selectedAccounts.size() > 1) currentDisplay += "*";
                        }
                    } else {
                        currentDisplay = "Ready";
                    }
                }

                std::string previousDisplay = g_state.previousText;
            	/*if (g_state.previousType == Type::Loading && g_state.isTransitioning) {
            		previousDisplay += GetAnimatedDots(time);
            	}*/

                if (wasIdle) {
                    if (hasSelectedAccounts) {
                        previousDisplay = "Selected: ";
                        for (std::size_t i = 0; i < selectedAccounts.size(); ++i) {
                            if (i > 0) previousDisplay += ", ";
                            previousDisplay += selectedAccounts[i].label;
                            if (i == 0 && selectedAccounts.size() > 1) previousDisplay += "*";
                        }
                    } else {
                        previousDisplay = "Ready";
                    }
                }

                const float textWidth = std::max(
                    ImGui::CalcTextSize(currentDisplay.c_str()).x,
                    g_state.isTransitioning ? ImGui::CalcTextSize(previousDisplay.c_str()).x : 0.0f
                );

                // Clip rect for roulette effect
                const ImVec2 clipMin(textStartPos.x, textStartPos.y);
                const ImVec2 clipMax(textStartPos.x + textWidth + 50.0f, textStartPos.y + lineHeight);

                drawList->PushClipRect(clipMin, clipMax, true);

                if (g_state.isTransitioning) {
                    const float easedProgress = EaseOutCubic(g_state.transitionProgress);

                    // Previous text scrolls up
                    const float prevOffset = -easedProgress * lineHeight;
                    ImVec4 prevTextColor = GetTextColor(g_state.previousType);
                    prevTextColor.w = 1.0f - easedProgress;

                    const ImVec2 prevPos(textStartPos.x, textStartPos.y + prevOffset);
                    drawList->AddText(prevPos, ImGui::ColorConvertFloat4ToU32(prevTextColor), previousDisplay.c_str());

                    // Current text scrolls up from below
                    const float currOffset = (1.0f - easedProgress) * lineHeight;
                    ImVec4 currTextColor = GetTextColor(g_state.currentType);
                    currTextColor.w = easedProgress;

                    const ImVec2 currPos(textStartPos.x, textStartPos.y + currOffset);
                    drawList->AddText(currPos, ImGui::ColorConvertFloat4ToU32(currTextColor), currentDisplay.c_str());
                } else {
                    ImVec4 textColor = GetTextColor(g_state.currentType);
                    drawList->AddText(textStartPos, ImGui::ColorConvertFloat4ToU32(textColor), currentDisplay.c_str());
                }

                drawList->PopClipRect();

                ImGui::Dummy(ImVec2(textWidth, lineHeight));
            }
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
    }
}