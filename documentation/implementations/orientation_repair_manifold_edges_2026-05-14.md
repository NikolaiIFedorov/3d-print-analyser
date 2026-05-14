# Planar face orientation repair (manifold same-directed edges) — 2026-05-14

## Problem

`AppInvalidTag::InconsistentFaceOrientation` is set when an `Edge` has **two** incident faces and both traverse it as the **same** directed pair `(start, end)` instead of opposing directions.

## Approach

- **`Face::FlipWindingIfPlanar()`** (`Face.hpp` / `Face.cpp`): for planar faces only, **reverse** each boundary loop’s `OrientedEdge` order and **toggle** each `reversed` flag, then **`CalculatePlanarData()`** so `PlanarSurface::data.normal` / `d` match the new winding.
- **`GeometryValidity::TryRepairInconsistentFaceOrientationSolid(Solid &)`**:
  - Build per-edge list of `(Face*, start, end)` from the solid’s face loops.
  - For each edge with **`dependencies.size() == 2`** and **exactly two** directed entries with **identical** `(a,b)`:
    - Require at least one incident face **planar**; if both planar, flip the one with the **larger** `Face*` address (stable tie-break); if only one planar, flip that one.
    - **Iterate** up to 128 passes until no same-directed manifold pairs remain (propagation when one flip fixes one edge but perturbs neighbors).
  - **Skips:** non–two-face edges, non-planar-only pairs, `NURBS` faces (flip is a no-op).

## Invocation

After **`TryRepairDegenerateSolidBRep`**, before **`RefreshSolidAppGeometryValidityCache`**, from `Scene::CreateSolid` and `Scene::MergeCoplanarFaces`.

## Limits

- Assumes **clean** manifold along the edge: exactly **two** faces on `Edge::dependencies` matching the two half-edge uses.
- Does not fix **non-manifold** or **open-boundary** orientation globally.
- **NURBS** same-directed pairs are left unchanged (no flip implementation yet).

## Outcome

Build clean; `LOG_BACK` when any pass changed winding.
