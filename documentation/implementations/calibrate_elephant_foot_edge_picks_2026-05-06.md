# Calibrate: Elephant’s foot via first-layer parallel edges (2026-05-06)

## Idea

Elephant’s foot should not activate from “any face fully in the first layer slab.” Instead: user picks **two parallel edges** on the **first-layer build-parallel cap**; nominal span is the perpendicular distance between those edge lines in the face plane.

## Plan

- Remove `ElephantFoot` outcome from `ClassifyFace` / `CombinePickedFaces` face-only paths.
- Detect `ElephantFoot` in `Display::RefreshCalibWorkflow` when both slots store the **same face**, both edges set, face is first-layer slab + layer cap ∥ build, edges on boundary and parallel.
- Extend `PickCalibrateAtPixel` to resolve an edge (ray→segment) **only** when the hit face qualifies as that cap in the first layer; use a modest mm² distance threshold so face-center clicks stay face picks.
- Fix second-pick validation: same-face parallel edges must bypass `NormalsAlignedForCalibPick` (it rejects `a == b`).
- Nominal + overlay: `CalibrateNominal::SpanPreviewBetweenParallelEdges` / `SpanBetweenParallelEdges`.

## Outcome

Shipped **elephant’s foot** as an explicit **two parallel edges on the same first-layer build-parallel cap** workflow:

- `ClassifyFace` / `CombinePickedFaces` no longer emit `ElephantFoot` from “face lies in first slab” alone; bottom faces behave as Contour/Hole for face–face picks.
- `PickCalibrateAtPixel` resolves a boundary **edge** when the ray hits such a cap in the first layer and the ray passes within **6 mm** of an edge on that face (`kCalibEdgePickMaxDistSqMm`).
- Second pick: if the first pick stored an edge on that cap, the second must be another edge on the **same face**, **parallel** (chord dot ≥ 0.985), distinct edge — bypasses `NormalsAlignedForCalibPick` same-face issue.
- Nominal span: `SpanBetweenParallelEdgesOnFace` / preview connector between parallel edge lines in the face plane.
- Invalid-face pool skipped while awaiting that second edge pick (misleading tint otherwise).

**Mini retro:** Thread-local scratch for filtered edge segments avoids per-hover heap allocation; threshold may need tuning per UI scale or ortho zoom later.

Follow-up: optional modifier to force **face-only** pick on the first-layer cap when edges are nearby; document layer height dependence for first-layer slab.
