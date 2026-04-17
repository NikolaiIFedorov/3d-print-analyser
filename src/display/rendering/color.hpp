#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "glm/glm.hpp"
#include "logic/Analysis/AnalysisTypes.hpp"

static const float BASE = 0.1;
static const float UI_BASE = BASE / 2.0f;
static const float STEP = 0.1f;
static const float UI_ACCENT_L_BOOST = 0.10f; // luminance offset applied in GetAccent to compensate for perceived darkening from saturation
static const float GRID_EXTENT = 256.0f;

static const float FACE = BASE + STEP;
static const float EDGE = FACE + STEP;
static const float POINT = EDGE + STEP;

static const glm::vec3 FACE_DEFAULT = glm::vec3(FACE, FACE, FACE);
static const glm::vec3 EDGE_DEFAULT = glm::vec3(EDGE, EDGE, EDGE);
static const glm::vec3 POINT_DEFAULT = glm::vec3(POINT, POINT, POINT);

struct Color
{
    // Convert HSL (h in degrees 0–360, s and l in 0–1) to linear RGB.
    // Only L is intended to vary across UI depth levels; H and S stay constant.
    static glm::vec3 HslToRgb(float h, float s, float l)
    {
        float c = (1.0f - std::abs(2.0f * l - 1.0f)) * s;
        float x = c * (1.0f - std::abs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
        float m = l - c * 0.5f;
        glm::vec3 rgb;
        if (h < 60.f)
            rgb = {c, x, 0};
        else if (h < 120.f)
            rgb = {x, c, 0};
        else if (h < 180.f)
            rgb = {0, c, x};
        else if (h < 240.f)
            rgb = {0, x, c};
        else if (h < 300.f)
            rgb = {x, 0, c};
        else
            rgb = {c, 0, x};
        return rgb + m;
    }

    // Set the accent hue (0–360) and saturation (0–1) at runtime.
    // Falls back to defaults if not called (e.g. non-macOS or achromatic system accent).
    static void SetAccent(float hue, float sat)
    {
        s_accentHue = hue;
        s_accentSat = sat;
    }

    // Switch between dark (default) and light appearance.
    // dark=true: UI steps from near-black upward; dark=false: steps from near-white downward.
    static void SetAppearance(bool dark)
    {
        s_darkMode = dark;
        s_uiBase = dark ? 0.05f : 0.95f;
    }

    static bool IsDark() { return s_darkMode; }

    static glm::vec3 GetBase()
    {
        // Sits just outside the UI depth scale (s_uiBase = 0.05 dark / 0.95 light)
        // so panels always have clear separation from the viewport background.
        float v = s_darkMode ? 0.03f : 0.97f;
        return glm::vec3(v, v, v);
    }
    static glm::vec4 GetUI(int depth, float alpha = 1.0f)
    {
        float v = s_darkMode ? s_uiBase + STEP * depth
                             : s_uiBase - STEP * depth;
        return glm::vec4(v, v, v, alpha);
    }
    static glm::vec4 GetInputBg(int layer, float alpha = 1.0f)
    {
        return GetUI(std::max(layer, 2), alpha);
    }
    static glm::vec4 GetUIText(int depth, float alpha = 1.0f)
    {
        float v = s_darkMode ? s_uiBase + STEP * (depth + 6)
                             : s_uiBase - STEP * (depth + 6);
        return glm::vec4(v, v, v, alpha);
    }
    // Accent color: same luminance progression as GetUI but with accent hue/saturation applied.
    // Use for structural elements (splitters) and interactive state feedback (hover, active).
    // satMult scales the system saturation (0=neutral grey, 1=full system saturation).
    static glm::vec4 GetAccent(int depth, float alpha = 1.0f, float satMult = 1.0f)
    {
        float l = s_darkMode ? s_uiBase + STEP * depth + UI_ACCENT_L_BOOST * satMult
                             : s_uiBase - STEP * depth - UI_ACCENT_L_BOOST * satMult;
        return glm::vec4(HslToRgb(s_accentHue, s_accentSat * satMult, l), alpha);
    }

    static glm::vec3 GetEdge()
    {
        float v = s_darkMode ? 0.30f : 0.70f;
        return glm::vec3(v, v, v);
    }
    static glm::vec3 GetPoint()
    {
        float v = s_darkMode ? 0.40f : 0.60f;
        return glm::vec3(v, v, v);
    }
    static glm::vec3 GetFace()
    {
        float v = s_darkMode ? 0.20f : 0.80f;
        return glm::vec3(v, v, v);
    }

    static glm::vec3 GetGrid()
    {
        float v = s_darkMode ? 0.22f : 0.84f;
        return glm::vec3(v, v, v);
    }
    static glm::vec3 GetAxisX(bool positive = true)
    {
        return positive ? glm::vec3(0.85f, 0.15f, 0.15f) : glm::vec3(0.50f, 0.10f, 0.10f);
    }
    static glm::vec3 GetAxisY(bool positive = true)
    {
        return positive ? glm::vec3(0.15f, 0.70f, 0.15f) : glm::vec3(0.10f, 0.40f, 0.10f);
    }
    static glm::vec3 GetAxisZ(bool positive = true)
    {
        return positive ? glm::vec3(0.15f, 0.35f, 0.90f) : glm::vec3(0.10f, 0.20f, 0.55f);
    }

    static glm::vec4 GetFace(FaceFlawKind flaw);
    static glm::vec4 GetEdge(EdgeFlawKind flaw);

private:
    inline static bool s_darkMode = true;     // dark by default
    inline static float s_uiBase = 0.05f;     // starting luminance (adjusted by SetAppearance)
    inline static float s_accentHue = 220.0f; // default: blue-gray
    inline static float s_accentSat = 0.35f;  // default saturation
};
