# Degenerate triangle repair (weld + face removal) — 2026-05-14

## Problem

`AppInvalidTag::DegenerateTriangle` flags near–zero area from the same fan test as `EvaluateAppInvalidTagsForSolid`. We wanted a first-pass fix: **(1) weld** nearby straight-edge endpoints, then **(2) remove** faces still degenerate by that rule.

## Approach

- **`GeometryValidity::TryRepairDegenerateSolidBRep`** (`include/GeometryValidity.hpp`, `src/logic/GeometryValidity.cpp`):
  - Collect edges referenced by the solid’s face loops.
  - **Constrained endpoints:** edges with `curve != nullptr` or non-empty `bridgePoints` — their endpoints are not welded (avoids moving NURBS / polyline anchors).
  - **Weld ε:** `max(float ULP noise × 16, 1e-9 × bbox extent, 1e-12)` from solid vertex bounds (similar spirit to binary STL weld).
  - **Spatial hash** + DSU on straight-edge endpoints only; canonical point per component = smallest `Point*` address.
  - **Retarget** straight edges: update `startPoint` / `endPoint` and `Point::dependencies`.
  - **Remove faces:** snapshot `solid.faces`, drop any face still failing `FaceStillDegenerateByFanRule` vs `MinFanTriangleAreaSquaredThreshold` (same `(1e-24 × scale)²` as evaluation). Clear loops, edge `dependencies`, `dependency = nullptr` (same defunct pattern as coplanar merge).
- **Invocation:** `Scene::CreateSolid` and end of `Scene::MergeCoplanarFaces`, before `RefreshSolidAppGeometryValidityCache`.
- **Logging:** `LOG_BACK` with weld pair count, edges retargeted, faces removed when anything changed.

## Limits / follow-ups

- Does not **merge duplicate `Edge*` objects** after weld (two `Edge` records between same `Point*` pair can remain).
- Does not recompute **planar face equations** after small vertex moves (ε is tiny; acceptable for now).
- **Non-convex n-gons:** fan-from-`v0` test matches evaluator but can be conservative vs true polygon area.
- **Curved-only solids:** weld only affects straight edges; degeneracy from sampling may need a different path.

**Tracked in** `documentation/TODO` under *Geometry validity & degenerate repair (optional hardening)*.

## Outcome

Build clean; repair is best-effort and idempotent-ish (second pass usually no-op unless new degeneracies appear).
