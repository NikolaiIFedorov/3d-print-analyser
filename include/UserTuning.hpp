#pragma once

#include <algorithm>

namespace UserTuning
{
// Grouped controls exposed in Settings UI.
inline float contrast = 0.5f; // 0..1
inline float snap = 0.3f;     // 0..1

// Derived runtime parameters used by renderer/camera.
// UI / appearance
inline float uiDepthStep = 0.10f;
inline float formHoverAlphaScale = 1.0f;

// Camera gesture-end snap threshold.
inline float snapEnterDeg = 3.0f;

inline float Mix(float a, float b, float t)
{
    return a + (b - a) * std::clamp(t, 0.0f, 1.0f);
}

inline void DeriveFromContrast()
{
    const float t = std::clamp(contrast, 0.0f, 1.0f);
    uiDepthStep = Mix(0.06f, 0.16f, t);
    formHoverAlphaScale = Mix(0.75f, 1.35f, t);
}

inline void DeriveFromSnap()
{
    const float t = std::clamp(snap, 0.0f, 1.0f);
    snapEnterDeg = Mix(1.5f, 6.0f, t);
}
} // namespace UserTuning
