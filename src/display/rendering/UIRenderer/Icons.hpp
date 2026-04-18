#pragma once

#include <functional>
#include <imgui.h>
#include <glm/glm.hpp>
#include "rendering/color.hpp"

// Programmatic icon drawing functions for the UI system.
//
// Each factory returns a DrawFn matching the SectionLine::iconDraw signature:
//   void(ImDrawList* dl, float x, float midY, float s)
//
// Parameters:
//   x     = left edge of the icon slot (content-left of the paragraph)
//   midY  = vertical centre of the line in screen pixels
//   s     = half-size of the icon slot (slot width = 2s, computed from font size × ICON_SIZE_RATIO)
//
// Icon slot convention (matches chevron): slot is 2s pixels wide, centred at (x + s, midY).
// Each icon should draw within a (2s × 2s) region centred at (x + s, midY).
//
// Stroke weight: use s * STROKE_RATIO for line thickness so icons scale consistently.

namespace Icons
{
    using DrawFn = std::function<void(ImDrawList *, float, float, float)>;

    // Shared stroke weight ratio: stroke = s * STROKE_RATIO
    constexpr float STROKE_RATIO = 0.22f;

    // --- Placeholder ---
    // A simple outlined square. Used while real icons are not yet designed.
    inline DrawFn Placeholder(int textDepth = 1)
    {
        return [textDepth](ImDrawList *dl, float x, float midY, float s)
        {
            glm::vec4 tc = Color::GetUIText(textDepth);
            ImU32 col = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
            float stroke = std::max(1.0f, s * STROKE_RATIO);
            float cx = x + s;
            float r = s * 0.72f; // square half-size — slightly inset from slot edge
            dl->AddRect(ImVec2(cx - r, midY - r), ImVec2(cx + r, midY + r), col, 0.0f, 0, stroke);
        };
    }

} // namespace Icons
