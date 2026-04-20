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
                ImVec2(cx - r, cy),                   // left wing
                ImVec2(cx + r * 0.5f, cy + r * 0.5f), // apex (bottom-right)
                ImVec2(cx, cy - r),                   // top wing
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
            float r = std::round(s * 0.70f);
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

    // --- Chevron ---
    // Right-pointing (collapsed) or down-pointing (expanded) chevron.
    // Used by UIRenderer to indicate collapsible section state.
    // iconSizeRatio should be set to ICON_SIZE_RATIO_SMALL on the owning SectionLine.
    inline DrawFn Chevron(bool expanded, int textDepth = 1)
    {
        return [expanded, textDepth](ImDrawList *dl, float x, float midY, float s)
        {
            glm::vec4 tc = Color::GetUIText(textDepth);
            ImU32 col = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
            float stroke = std::max(0.8f, s * STROKE_RATIO);
            float cx = std::round(x + s);
            float cy = std::round(midY);
            float r = std::round(s * 0.60f);
            ImVec2 pts[3];
            if (expanded)
            {
                // Down-pointing V: left-top → apex-bottom → right-top
                pts[0] = ImVec2(cx - r, cy - r * 0.55f);
                pts[1] = ImVec2(cx, cy + r * 0.55f);
                pts[2] = ImVec2(cx + r, cy - r * 0.55f);
            }
            else
            {
                // Right-pointing >: left-top → apex-right → left-bottom
                pts[0] = ImVec2(cx - r * 0.55f, cy - r);
                pts[1] = ImVec2(cx + r * 0.55f, cy);
                pts[2] = ImVec2(cx - r * 0.55f, cy + r);
            }
            dl->AddPolyline(pts, 3, col, 0, stroke);
        };
    }

    // --- ThemeSystem ---
    // Monitor outline with a small stand — represents "follow system" mode.
    inline DrawFn ThemeSystem(glm::vec4 color = {0, 0, 0, 0}, int textDepth = 1)
    {
        return [color, textDepth](ImDrawList *dl, float x, float midY, float s)
        {
            glm::vec4 tc = color.a > 0.0f ? color : Color::GetUIText(textDepth);
            ImU32 col = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
            float stroke = std::max(0.8f, s * STROKE_RATIO);
            float cx = std::round(x + s);
            float cy = std::round(midY);
            float rw = std::round(s * 0.72f); // screen half-width
            float rh = std::round(s * 0.52f); // screen half-height
            float bot = cy + rh;
            // Screen rect
            dl->AddRect(ImVec2(cx - rw, cy - rh), ImVec2(cx + rw, bot), col, 0.0f, 0, stroke);
            // Stand: short vertical nub + wider base
            float nubH = std::round(s * 0.22f);
            float baseW = std::round(s * 0.40f);
            float baseH = std::max(0.8f, stroke * 0.85f);
            dl->AddLine(ImVec2(cx, bot), ImVec2(cx, bot + nubH), col, stroke);
            dl->AddRectFilled(ImVec2(cx - baseW, bot + nubH - baseH), ImVec2(cx + baseW, bot + nubH + baseH), col);
        };
    }

    // --- ThemeLight ---
    // Filled circle (sun disc) with eight short radiating strokes.
    inline DrawFn ThemeLight(glm::vec4 color = {0, 0, 0, 0}, int textDepth = 1)
    {
        return [color, textDepth](ImDrawList *dl, float x, float midY, float s)
        {
            glm::vec4 tc = color.a > 0.0f ? color : Color::GetUIText(textDepth);
            ImU32 col = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
            float stroke = std::max(0.8f, s * STROKE_RATIO);
            float cx = std::round(x + s);
            float cy = std::round(midY);
            float disc = std::round(s * 0.30f);
            float r0 = std::round(s * 0.48f); // ray inner radius
            float r1 = std::round(s * 0.68f); // ray outer radius
            dl->AddCircleFilled(ImVec2(cx, cy), disc, col, 12);
            for (int i = 0; i < 8; ++i)
            {
                float a = static_cast<float>(i) * 3.14159265f / 4.0f;
                float ca = std::cos(a), sa = std::sin(a);
                dl->AddLine(ImVec2(cx + ca * r0, cy + sa * r0),
                            ImVec2(cx + ca * r1, cy + sa * r1), col, stroke);
            }
        };
    }

    // --- ThemeDark ---
    // Crescent moon: large circle minus a smaller offset circle, drawn as a filled polygon.
    inline DrawFn ThemeDark(glm::vec4 color = {0, 0, 0, 0}, int textDepth = 1)
    {
        return [color, textDepth](ImDrawList *dl, float x, float midY, float s)
        {
            glm::vec4 tc = color.a > 0.0f ? color : Color::GetUIText(textDepth);
            ImU32 col = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
            float cx = std::round(x + s);
            float cy = std::round(midY);
            float r = s * 0.62f;
            // Outer disc
            dl->AddCircleFilled(ImVec2(cx, cy), r, col, 24);
            // Cut-out: slightly smaller, offset up-right, using background color to punch the crescent.
            glm::vec4 bg = Color::GetUI(1); // panel background depth
            ImU32 bgCol = ImGui::GetColorU32(ImVec4(bg.r, bg.g, bg.b, bg.a));
            float cutR = r * 0.72f;
            float offX = r * 0.38f;
            float offY = -r * 0.32f;
            dl->AddCircleFilled(ImVec2(cx + offX, cy + offY), cutR, bgCol, 24);
        };
    }

    // --- AccentCustom ---
    // Pencil outline at 45° (eraser top-right, chisel tip bottom-left).
    // Conveys "custom / user-defined" for the accent colour selector.
    // Flat chisel tip instead of a sharp point so the icon reads cleanly at small sizes.
    inline DrawFn AccentCustom(int textDepth = 1)
    {
        return [textDepth](ImDrawList *dl, float x, float midY, float s)
        {
            glm::vec4 tc = Color::GetUIText(textDepth);
            ImU32 col = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
            float stroke = std::max(0.8f, s * STROKE_RATIO);
            float cx = std::round(x + s);
            float cy = std::round(midY);
            float r = s * 0.68f;

            constexpr float k = 0.7071f; // 1/√2 — axis is 45°
            float hw = r * 0.30f;        // body half-width
            float hw_tip = r * 0.08f;    // chisel tip half-width (flat, not pointed)

            // Axis: eraser top-right → tip bottom-left
            float erx = cx + r * k, ery = cy - r * k;
            float tx = cx - r * k, ty = cy + r * k;

            // Shaft end: where body tapers toward tip (65% from tip to eraser)
            float bex = tx + (erx - tx) * 0.65f;
            float bey = ty + (ery - ty) * 0.65f;

            // Perpendicular offset vectors (rotate axis 90° CW: (-k,+k) → (+k,+k))
            float ox = k * hw, oy = k * hw;
            float ox_t = k * hw_tip, oy_t = k * hw_tip;

            // 6-point closed shape: eraser rect + tapered shaft + flat chisel tip
            ImVec2 pts[6] = {
                ImVec2(erx + ox, ery + oy),   // eraser A
                ImVec2(erx - ox, ery - oy),   // eraser B
                ImVec2(bex - ox, bey - oy),   // shaft B (near tip)
                ImVec2(tx - ox_t, ty - oy_t), // chisel tip B
                ImVec2(tx + ox_t, ty + oy_t), // chisel tip A
                ImVec2(bex + ox, bey + oy),   // shaft A (near tip)
            };
            dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, stroke);
        };
    }

} // namespace Icons
