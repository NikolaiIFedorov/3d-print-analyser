#pragma once

namespace UserTuning
{
// UI / appearance
inline float uiDepthStep = 0.10f;

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
} // namespace UserTuning
