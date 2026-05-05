# Calibrate: wall ⊥ build + span ⊥ build + hole sidewall ≥2 edges (2026-05-06)

## Problem

Contour/hole compensation used single scalars without enforcing that CAD spans matched **layer-plane** (horizontal) intent; slanted picks mixed axes. Hole sidewalls could be tagged from a single inner-loop edge, weak vs exterior vertical walls.

## Approach

1. **`FaceNormalPerpendicularToBuild`** — planar faces with `|dot(n̂, buildDir̂)| ≤ 0.15` (≈ vertical wall for +Z build).
2. **Pick gate** (`CalibFacePickPassesWallGate` in `display.cpp`) — non–elephant picks must pass wall gate **or** first-layer cap + edge snap (elephant’s foot).
3. **`CalibSecondPickAcceptsHit`** — after elephant branch, both resolved faces must pass wall gate; then existing normal-align + workflow compatibility.
4. **Nominal span** — `NominalSpanPerpendicularToBuild` uses `SpanPreviewBetweenFaces` segment vs build axis with same 0.15 bound; invalidates Contour/Hole in `RefreshCalibWorkflow` and `RefreshCalibCompensation` (elephant foot exempt).
5. **`FaceQualifiesAsHole` sidewalls** — ⊥ build **and** **≥2** boundary edges each **witnessed** via `EdgeBordersStackParallelHoleOpening` (edge lies on an inner loop of a planar cap ∥ build — opening parallel slice planes, tunnel ∥ stack). Annulus caps ∥ build unchanged.
6. **Invalid-face pool** — tint faces failing wall gate during second pick (non–elephant path).

## Outcome

Shipped; Calibrate panel copy updated for ⊥ +Z picks and elephant exception.

## 2026-05-06 follow-up — hole orientation witness

Sidewalls were distinguished from contour walls using inner-loop edge hashes only; orientation (“opening parallel layers”) is now explicit: `EdgeBordersStackParallelHoleOpening` walks edge-incident faces and requires membership on **inner** loops of **stack-parallel** caps. `layerHoleInnerEdges` remains built for tooling/other callers; sidewall classification no longer depends on set membership.

## Mini retro

Constants shared via `CalibDistanceType.hpp` (`kWallNormalMaxAbsDotBuild`, `kCalibSpanMaxAbsDotBuildAxis`). Tune if printers/build conventions change.
