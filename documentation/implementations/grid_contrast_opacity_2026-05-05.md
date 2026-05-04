# Grid contrast linkage + opacity setting (2026-05-05)

## Idea

- **Issue:** Appearance “Contrast” only drove `Color::Step()` (`uiDepthStep`) via `UserTuning::DeriveFromContrast()`; viewport grid RGB came from fixed `Color::GetGrid()`, so sliders had no visible effect on the grid.
- **Also:** Plane-tilt already modulates fragment alpha internally; exposing a persisted **viewport grid opacity** multiplier gives direct control without duplicating tilt logic.

## Plan

1. Derive grid gray from the same contrast-driven `Step()` as mesh/wire neutrals (anchored near previous mid-contrast luminance vs `GetBase()`).
2. `RegenerateGrid()` when contrast changes (vertex colors are baked).
3. `Settings::gridOpacity` [0–1], UI under Viewport, applied as `ViewportRenderer::SetGridOpacityUserScale` multiplied with existing tilt ramp (clamped).

## Outcome

- Implemented in `color.hpp`, `ViewportRenderer.{hpp,cpp}`, `Settings.hpp`, `display.cpp`.
- Build: cmake build OK.

## Mini retro

- Keeping grid as rebuilt line colors (vs uniforms) avoids shader churn but requires invalidation whenever contrast morphs grayscale.
