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
}
