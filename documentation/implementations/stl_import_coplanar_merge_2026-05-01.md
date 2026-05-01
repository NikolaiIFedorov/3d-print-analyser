# STL import — coplanar face merge refinement (2026-05-01)

## Problem

STL import creates one face per triangle, then `Scene::MergeCoplanarFaces` merges adjacent coplanar facets. Users see cases where triangles on the same physical surface stay split.

## Root causes (analysis)

1. **Exact vertex keys**: `std::map<glm::dvec3, Point*>` uses exact double equality. Slightly different coordinates for the same logical vertex break edge sharing; merge only follows shared edges.
2. **Plane test**: merge required `|di.d - dj.d| <= 1e-4` for parallel normals. Per-triangle `d` from different vertex references and cross-product normals can drift beyond that while geometry remains coplanar.

## Plan

- Binary STL: first pass over triangle vertices for axis-aligned bbox; derive a small weld epsilon from diagonal; quantize positions before the point map (snap to grid).
- ASCII STL: fixed conservative weld epsilon (no full-file bbox pass to keep a single readable pass).
- `MergeCoplanarFaces`: replace strict `d` delta with max vertex distance to the other face’s plane (symmetric), with `planeTol` derived from solid bbox (`clamp(1e-7 * diagonal, …)`).

## Outcome

- Binary STL: bounds pass over all triangle vertices, then `weldEps = clamp(1e-7 * diagonal, 1e-10, 1e-3)`; positions snapped to that grid before the point map so shared edges exist when coordinates differ by float noise.
- ASCII STL: fixed `weldEps = 1e-6` (single pass).
- `MergeCoplanarFaces`: coplanarity uses symmetric max vertex–plane distance with `planeTol = clamp(1e-7 * diagonal, 1e-10, 1.0)` instead of `|d_i - d_j| <= 1e-4`.
- Clean build (`cmake --build build`).

## Mini retro

- Pairing **vertex welding** with a **geometry-based plane test** addresses the two dominant failure modes (no shared edge vs. per-triangle plane constant drift) without a full mesh topology repair pass.
- Follow-up if needed: optional user tolerance, or ASCII bbox pre-scan for adaptive weld like binary.
