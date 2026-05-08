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

## 2026-05 — normal slack for merge (sliver walls)

- `MergeCoplanarFaces` gated coplanarity on `dot(ni,nj) > 1 - normalTolerance` with hardcoded **1e-3** (~2.6°); skinny wall triangles yield unstable normals and stayed split.
- **Change:** `GeometryExperiments::kMergeCoplanarNormalDotSlack` **5e-3** (~5.7°), single place to tweak (`include/GeometryExperiments.hpp`).

## 2026-05 follow-up — merge diagnostics → `session_log.json`

- **`MergeCoplanarDiagnostics`** collected in `Scene::MergeCoplanarFaces` (and `CollectCoplanarMergeTopology` when merge experiment skips merge).
- STL import stores them in **`STLImportStats`**; after import, **`SessionLogger::LogStlMergeDiagnostics`** emits event type **`stl_merge_diagnostics`** (face/edge histograms before & after, merge sweep counts, boundary-loop failures, bbox diagonal, plane tolerance).
- Written on the main thread after async import finalizes (same point as `file_import`), and on the legacy synchronous import path.

## 2026-05 follow-up — binary STL float-aware weld

### Idea

The diagonal-derived snap grid can miss duplicated STL seam vertices when two independently exported float32 coordinates are only a few ULPs apart but land on opposite quantization buckets. Binary STL is already float32, so use a tolerance derived from float precision instead of a user-selected grid accuracy.

### Plan

- Keep ASCII STL on the existing conservative text-path quantization.
- Replace binary STL's rounded diagonal grid with a small spatial weld map.
- Derive weld tolerance from `2 * FLT_EPSILON * maxAbsCoordinate`, then search neighboring cells so values near a cell boundary still weld when they are within tolerance.

### Outcome

- Binary STL import now welds vertices by nearby float32-scale position instead of snapping them to a global diagonal grid.
- Created points keep the original imported coordinate from the first matching vertex; later matches only reuse the point when all coordinate deltas are within the float-aware tolerance.
- Clean build: `cmake --build build`.

### Mini retro

- What worked: treating binary STL noise as float32-scale data kept the tolerance tied to the file format instead of another user-facing accuracy knob.
- What to watch: if real models contain intentional gaps below a few float ULPs at their coordinate scale, this weld may still be too permissive; session diagnostics should make that visible through unique point / shared-edge changes.

## 2026-05 follow-up — plane tolerance floor

### Idea

Remaining unmerged facets after float-aware vertex welding may be passing topology but failing the planar distance check. For mm-scale print CAD, a `0.001` model-unit floor is still far below normal printable feature size while giving exporter / float drift more room than the current diagonal-derived value on medium models.

### Outcome

- Added `GeometryExperiments::kMergeCoplanarPlaneTolFloor = 1e-3`.
- `MergePlaneTolFromDiagonal` now uses the maximum of the existing geometric / float / import tolerances and that explicit floor.
