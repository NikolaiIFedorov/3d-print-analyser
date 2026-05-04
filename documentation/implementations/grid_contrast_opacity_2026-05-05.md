# Grid contrast linkage + opacity setting (2026-05-05)

## Idea

- **Issue:** Appearance “Contrast” only drove `Color::Step()` (`uiDepthStep`) via `UserTuning::DeriveFromContrast()`; viewport grid RGB came from fixed `Color::GetGrid()`, so sliders had no visible effect on the grid.
- **Also:** Plane-tilt already modulates fragment alpha; the first persisted knob multiplied the whole tilt curve, which duplicated “opacity” wording without tuning **shallow-orbit fading** specifically.

### Follow-up (same thread)

Renamed semantics: **`gridPlaneTiltMinOpacity`** (XML `gridPlaneTiltMinOpacity`) sets the minimum of `GridOpacityFromPlaneTilt` when orbit is shallow (near-parallel to XY), default **0.5** to match the old baked constant. Loads legacy **`gridOpacity`** as **`legacy * 0.5`** if the new key is absent. UI label: **Low-angle grid opacity**.

## Plan

1. Derive grid gray from the same contrast-driven `Step()` as mesh/wire neutrals (anchored near previous mid-contrast luminance vs `GetBase()`).
2. `RegenerateGrid()` when contrast changes (vertex colors are baked).
3. Persist **`Settings::gridPlaneTiltMinOpacity`** [0–1]: floor on the tilt alpha ramp (still ramps to opaque for toward-top-down views).

## Outcome

- Implemented in `color.hpp`, `ViewportRenderer.{hpp,cpp}`, `Settings.hpp`, `display.cpp`.
- Build OK after refactor: persisted knob now drives **`GridOpacityFromPlaneTilt` minimum** (`gridPlaneTiltMinOpacity` / UI **Low-angle grid opacity**) instead of a uniform multiplier.
## Mini retro

- Keeping grid as rebuilt line colors (vs uniforms) avoids shader churn but requires invalidation whenever contrast morphs grayscale.
