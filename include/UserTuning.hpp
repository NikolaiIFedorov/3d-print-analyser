#pragma once

#include <algorithm>

namespace UserTuning
{
// Grouped controls exposed in Settings UI.
inline float contrast = 0.5f; // 0..1
inline float lod = 0.5f;      // 0..1
inline float snap = 0.3f;     // 0..1

// Derived runtime parameters used by renderer/camera.
// UI / appearance
inline float uiDepthStep = 0.10f;
inline float formHoverAlphaScale = 1.0f;

// Viewport grid LOD
inline float gridLodMinPixelGap = 1.0f;
inline float gridForeshortenFloor = 0.055f;
inline float gridForeshortenExponent = 1.28f;
inline float gridLodHysteresisBand = 1.06f;
inline float gridLodMinWorldStep = 1.0f / 256.0f;
inline float gridLodMaxWorldStep = 32.0f;

// Camera snap hysteresis
inline float snapEnterDeg = 3.0f;
inline float snapExitDeg = 5.25f;

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

inline void DeriveFromLod()
{
    const float t = std::clamp(lod, 0.0f, 1.0f);
    gridLodMinPixelGap = Mix(0.70f, 1.80f, t);
    gridForeshortenFloor = Mix(0.09f, 0.03f, t);
    gridForeshortenExponent = Mix(1.0f, 1.65f, t);
    gridLodHysteresisBand = Mix(1.02f, 1.12f, t);
    gridLodMinWorldStep = Mix(1.0f / 512.0f, 1.0f / 128.0f, t);
    gridLodMaxWorldStep = Mix(16.0f, 96.0f, t);
    if (gridLodMaxWorldStep < gridLodMinWorldStep)
        gridLodMaxWorldStep = gridLodMinWorldStep;
}

inline void DeriveFromSnap()
{
    const float t = std::clamp(snap, 0.0f, 1.0f);
    snapEnterDeg = 3.0f;                    // keep snap-in feel stable
    snapExitDeg = snapEnterDeg + Mix(0.4f, 4.0f, t);
}
} // namespace UserTuning
