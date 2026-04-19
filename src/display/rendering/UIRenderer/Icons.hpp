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

    // Shared stroke weight ratio: stroke = s * STROKE_RATIO, floored at 0.8px (anti-aliased sub-pixel)
    constexpr float STROKE_RATIO = 0.15f;

    // --- ImportFile ---
    // Simple plus sign — two filled bars crossing at centre.
    inline DrawFn ImportFile(glm::vec4 color = {0, 0, 0, 0}, int textDepth = 1)
    {
        return [color, textDepth](ImDrawList *dl, float x, float midY, float s)
        {
            glm::vec4 tc = color.a > 0.0f ? color : Color::GetUIText(textDepth);
            ImU32 col = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
            float cx = std::round(x + s);
            float cy = std::round(midY);
            float r = std::round(s * 0.70f);
            float bar = std::max(0.8f, std::round(s * STROKE_RATIO));                   // half-thickness of each arm
            dl->AddRectFilled(ImVec2(cx - r, cy - bar), ImVec2(cx + r, cy + bar), col); // horizontal
            dl->AddRectFilled(ImVec2(cx - bar, cy - r), ImVec2(cx + bar, cy + r), col); // vertical
        };
    }

    // --- Placeholder ---
    // A simple outlined square. Used while real icons are not yet designed.
    inline DrawFn Placeholder(int textDepth = 1)
    {
        return [textDepth](ImDrawList *dl, float x, float midY, float s)
        {
            glm::vec4 tc = Color::GetUIText(textDepth);
            ImU32 col = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
            float stroke = std::max(0.8f, s * STROKE_RATIO);
            float cx = x + s;
            float r = s * 0.72f; // square half-size — slightly inset from slot edge
            dl->AddRect(ImVec2(cx - r, midY - r), ImVec2(cx + r, midY + r), col, 0.0f, 0, stroke);
        };
    }

    // --- Overhang ---
    // L cross-section: solid horizontal arm at top + solid vertical arm on the left.
    // Reads as the side profile of an overhanging face — 3D in nature.
    // color: explicit RGBA override (alpha>0 uses it; alpha==0 falls back to GetUIText(textDepth)).
    inline DrawFn Overhang(glm::vec4 color = {0, 0, 0, 0}, int textDepth = 1)
    {
        return [color, textDepth](ImDrawList *dl, float x, float midY, float s)
        {
            glm::vec4 tc = color.a > 0.0f ? color : Color::GetUIText(textDepth);
            ImU32 col = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
            float cx = x + s;
            float r = s * 0.70f;
            float bar = r * 0.35f; // arm thickness
            // Two overlapping filled rects form the L
            dl->AddRectFilled(ImVec2(cx - r, midY - r), ImVec2(cx + r, midY - r + bar), col); // horizontal arm
            dl->AddRectFilled(ImVec2(cx - r, midY - r), ImVec2(cx - r + bar, midY + r), col); // vertical arm
        };
    }

    // --- SharpCorner ---
    // V rotated 45° CW: apex points bottom-right, wings extend left and upward.
    // Reads as a corner angle rather than the letter 'v'.
    // Derived by rotating (−r,−r),(0,+r),(+r,−r) by 45° CW then scaling by 1/√2
    // to keep all points within ±r: gives (−r,0),(+0.5r,+0.5r),(0,−r).
    inline DrawFn SharpCorner(glm::vec4 color = {0, 0, 0, 0}, int textDepth = 1)
    {
        return [color, textDepth](ImDrawList *dl, float x, float midY, float s)
        {
            glm::vec4 tc = color.a > 0.0f ? color : Color::GetUIText(textDepth);
            ImU32 col = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
            float stroke = std::max(0.8f, s * STROKE_RATIO);
            float cx = std::round(x + s);
            float cy = std::round(midY);
            float r = std::round(s * 0.70f);
            ImVec2 pts[3] = {
                ImVec2(cx - r,        cy),             // left wing
                ImVec2(cx + r * 0.5f, cy + r * 0.5f), // apex (bottom-right)
                ImVec2(cx,            cy - r),          // top wing
            };
            dl->AddPolyline(pts, 3, col, 0, stroke);
        };
    }

    // --- ThinSection ---
    // Single filled horizontal bar — solid thin cross-section of material.
    // Width spans the full slot; height is small to convey thinness.
    inline DrawFn ThinSection(glm::vec4 color = {0, 0, 0, 0}, int textDepth = 1)
    {
        return [color, textDepth](ImDrawList *dl, float x, float midY, float s)
        {
            glm::vec4 tc = color.a > 0.0f ? color : Color::GetUIText(textDepth);
            ImU32 col = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
            float cx = x + s;
            float r  = std::round(s * 0.70f);
            float barH = std::max(1.5f, std::round(r * 0.28f)); // half-height of the bar
            dl->AddRectFilled(ImVec2(cx - r, midY - barH), ImVec2(cx + r, midY + barH), col);
        };
    }

    // --- SmallFeature ---
    // Circle outline — represents a small hole or pinhole feature.
    inline DrawFn SmallFeature(glm::vec4 color = {0, 0, 0, 0}, int textDepth = 1)
    {
        return [color, textDepth](ImDrawList *dl, float x, float midY, float s)
        {
            glm::vec4 tc = color.a > 0.0f ? color : Color::GetUIText(textDepth);
            ImU32 col = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
            float cx = x + s;
            float stroke = std::max(0.8f, s * STROKE_RATIO);
            float r = s * 0.58f; // circle radius — slightly inset from slot edge
            dl->AddCircle(ImVec2(cx, midY), r, col, 0, stroke);
        };
    }

    // --- LayerHeight ---
    // Three solid horizontal bands representing stacked print layers.
    inline DrawFn LayerHeight(glm::vec4 color = {0, 0, 0, 0}, int textDepth = 1)
    {
        return [color, textDepth](ImDrawList *dl, float x, float midY, float s)
        {
            glm::vec4 tc = color.a > 0.0f ? color : Color::GetUIText(textDepth);
            ImU32 col = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
            float cx = x + s;
            float r = s * 0.70f;
            float spacing = r * 0.60f;
            float barHalf = r * 0.11f; // half-height of each band
            for (int i = -1; i <= 1; ++i)
            {
                float ly = midY + i * spacing;
                dl->AddRectFilled(ImVec2(cx - r, ly - barHalf), ImVec2(cx + r, ly + barHalf), col);
            }
        };
    }

} // namespace Icons
