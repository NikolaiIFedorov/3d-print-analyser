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

    static glm::vec3 GetBase() { return glm::vec3(BASE, BASE, BASE); }
    static glm::vec4 GetUI(int depth, float alpha = 1.0f)
    {
        float v = UI_BASE + STEP * depth;
        return glm::vec4(v, v, v, alpha);
    }
    static glm::vec4 GetInputBg(int layer, float alpha = 1.0f)
    {
        return GetUI(std::max(layer, 2), alpha);
    }
    static glm::vec4 GetUIText(int depth, float alpha = 1.0f)
    {
        float v = UI_BASE + STEP * (depth + 5);
        return glm::vec4(v, v, v, alpha);
    }
    // Accent color: same luminance progression as GetUI but with accent hue/saturation applied.
    // Use for structural elements (splitters) and interactive state feedback (hover, active, underlines).
    static glm::vec4 GetAccent(int depth, float alpha = 1.0f)
    {
        float l = UI_BASE + STEP * depth + UI_ACCENT_L_BOOST;
        return glm::vec4(HslToRgb(s_accentHue, s_accentSat, l), alpha);
    }

    static glm::vec3 GetEdge()
    {
        return EDGE_DEFAULT;
    }
    static glm::vec3 GetPoint()
    {
        return POINT_DEFAULT;
    }
    static glm::vec3 GetFace()
    {
        return FACE_DEFAULT;
    }

    static glm::vec3 GetGrid() { return glm::vec3(BASE + STEP, BASE + STEP, BASE + STEP); }
    static glm::vec3 GetAxisX(bool positive = true)
    {
        float v = positive ? BASE + 5 * STEP : BASE + 3 * STEP;
        return glm::vec3(v, BASE + STEP, BASE + STEP);
    }
    static glm::vec3 GetAxisY(bool positive = true)
    {
        float v = positive ? BASE + 5 * STEP : BASE + 3 * STEP;
        return glm::vec3(BASE + STEP, v, BASE + STEP);
    }
    static glm::vec3 GetAxisZ(bool positive = true)
    {
        float v = positive ? BASE + 5 * STEP : BASE + 3 * STEP;
        return glm::vec3(BASE + STEP, BASE + STEP, v);
    }

    static glm::vec4 GetFace(FaceFlawKind flaw);
    static glm::vec4 GetEdge(EdgeFlawKind flaw);

private:
    inline static float s_accentHue = 220.0f; // default: blue-gray
    inline static float s_accentSat = 0.35f;  // default saturation
};
