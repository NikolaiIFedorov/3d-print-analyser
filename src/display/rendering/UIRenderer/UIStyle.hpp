#pragma once

#include <imgui.h>
#include <glm/glm.hpp>
#include "rendering/color.hpp"

// Shared style constants and push/pop helpers for interactive input elements
// (DragFloat sliders and framed buttons). Centralises all visual parameters so
// both widget types stay consistent.
namespace UIStyle
{
    // ── Text roles ────────────────────────────────────────────────────────────
    // Describes the semantic purpose of a SectionLine, driving font and scale.
    // Color is controlled separately via SectionLine::textDepth.
    //   Title       — panel/section headers; uses bodyImFont at its given fontScale
    //   Normal      — body text, paragraph content; bodyImFont, 1.0x scale
    //   Small       — subtitles, contextual "why" lines; bodyImFont, 0.85x scale (owned by role)
    //   Value       — numeric readouts, units; bodyImFont, 1.0x scale
    //
    // Interactive override: if SectionLine::onClick or ::imguiContent is set, the
    // renderer replaces the resolved font with pixelImFont regardless of role.
    enum class TextRole
    {
        Title,
        Normal,
        Small,
        Value
    };

    struct TextStyle
    {
        TextRole role = TextRole::Normal;
    };

    constexpr float SmallScale() { return 0.85f; }

    // Returns the role-driven scale multiplier. Combined with SectionLine::fontScale.
    constexpr float RoleScale(TextRole role)
    {
        return role == TextRole::Small ? SmallScale() : 1.0f;
    }

    constexpr float FRAME_ROUNDING_RATIO = 0.3f;
    constexpr float ACCENT_SAT_MULT_HOVER = 0.6f; // interactive feedback: hover/active tint

    // Push ImGui style for a DragFloat (or any framed widget).
    // h  = widget height in pixels (used to derive rounding)
    // tc = text color
    inline void PushInputStyle(float h, glm::vec4 tc)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, h * FRAME_ROUNDING_RATIO);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(tc.r, tc.g, tc.b, tc.a));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    }

    inline void PopInputStyle()
    {
        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(4);
    }

    // Draw a hover/active background tint for the last ImGui item (e.g. DragFloat).
    // Call immediately after the widget. layer = UIElement::layer of the containing Paragraph.
    inline void DrawInputHoverTint(int layer)
    {
        bool hovered = ImGui::IsItemHovered();
        bool active = ImGui::IsItemActive();
        if (hovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (!hovered && !active)
            return;
        int d = active ? layer + 2 : layer + 1;
        glm::vec4 bg = Color::GetAccent(d, active ? 0.18f : 0.10f, ACCENT_SAT_MULT_HOVER);
        float radius = ImGui::GetItemRectSize().y * FRAME_ROUNDING_RATIO;
        ImVec2 rMin = ImGui::GetItemRectMin();
        ImVec2 rMax = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(rMin, rMax,
                                                  ImGui::GetColorU32(ImVec4(bg.r, bg.g, bg.b, bg.a)), radius);
    }
}
