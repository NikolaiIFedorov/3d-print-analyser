#pragma once

#include <imgui.h>
#include <glm/glm.hpp>
#include "rendering/color.hpp"

// Shared style constants and push/pop helpers for interactive input elements
// (DragFloat sliders and framed buttons). Centralises all visual parameters so
// both widget types stay consistent.
namespace UIStyle
{
    constexpr float FRAME_ROUNDING_RATIO = 0.3f;

    inline ImVec4 FrameBgHoveredColor()
    {
        auto c = Color::GetAccent(3, 0.5f);
        return {c.r, c.g, c.b, c.a};
    }
    inline ImVec4 FrameBgActiveColor()
    {
        auto c = Color::GetAccent(1, 0.5f);
        return {c.r, c.g, c.b, c.a};
    }

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

    // Draw an accent underline below the last ImGui item.
    // restDepth should match the parent element's nesting level; hover/active step up from there.
    inline void DrawInputUnderline(int restDepth)
    {
        ImVec2 rmin = ImGui::GetItemRectMin();
        ImVec2 rmax = ImGui::GetItemRectMax();
        float ulY = std::round(rmax.y) + 1.5f;
        int depth = ImGui::IsItemActive() ? restDepth + 2 : (ImGui::IsItemHovered() ? restDepth + 1 : restDepth);
        glm::vec4 ac = Color::GetAccent(depth);
        ImU32 ulCol = ImGui::GetColorU32(ImVec4(ac.r, ac.g, ac.b, ac.a));
        ImGui::GetWindowDrawList()->AddLine(ImVec2(rmin.x, ulY), ImVec2(rmax.x, ulY), ulCol, 2.0f);
    }
}
