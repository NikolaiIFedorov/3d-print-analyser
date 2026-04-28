# Scene dirty-update architecture (2026-04-29)

## Problem

- Scene updates currently use one broad invalidation path: `Display::UpdateScene()` marks everything dirty.
- Style-only changes (accent/theme/contrast) can trigger full scene mesh rebuild and full GPU re-upload.
- This same coarse path will not scale once interactive scene edits start modifying subsets of geometry.

## Plan

- Split invalidation domains in `Display` into style/geometry/pick buckets.
- Refactor `SceneRenderer` into explicit flows:
  - full rebuild,
  - dirty-solid partial rebuild,
  - style-only recolor refresh.
- Add per-solid generation APIs in `Wireframe` and `Patch`.
- Add range-based GPU subdata update methods with full-upload fallback.
- Route frame update order through targeted invalidation handling.
- Add instrumentation counters to verify behavior and frequency of full vs partial updates.

## Notes during implementation

- Added explicit invalidation domains in `Display`:
  - `styleDirty`,
  - `geometryDirtyAll`,
  - `geometryDirtySolids`,
  - `pickDirty`,
  with helper markers (`MarkStyleDirty`, `MarkGeometryDirtyAll`, `MarkGeometryDirtySolid`, `MarkPickDirty`).
- Refactored `SceneRenderer` into:
  - `RebuildAll`,
  - `RebuildSolids`,
  - `RecolorOnly`,
  backed by per-solid CPU chunk caches.
- Added per-solid and loose generation entry points in:
  - `Wireframe::{GenerateSolid, GenerateLoose}`,
  - `Patch::{GenerateSolid, GenerateLoose}`.
- Added range update APIs in `OpenGLRenderer`:
  - `UpdateTriangleMeshSubData(...)`,
  - `UpdateLineMeshSubData(...)`,
  with full-upload fallback whenever chunk layout changes.
- Routed appearance updates (accent/theme/contrast) to style + pick invalidation instead of broad geometry invalidation.
- Added runtime counters on `SceneRenderer` (`full`, `partial`, `recolor`) and surfaced them in status text.

## Outcome

- Build passes: `cmake --build build -j6`.
- Style-only updates no longer force unconditional full mesh/index uploads; recolor path prefers subdata updates and falls back safely.
- Geometry path now supports partial solid rebuild flow and preserves a compatibility full rebuild path.

## Mini retro

- What worked: introducing explicit invalidation classes first made follow-up refactors predictable and reduced coupling between UI events and mesh generation.
- What did not: recolor-only currently still regenerates chunk topology on CPU to validate layout; future work can store richer per-face/edge color spans to avoid that cost.
