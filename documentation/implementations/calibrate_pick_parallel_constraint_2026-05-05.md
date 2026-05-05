# Calibrate second pick — parallel face constraint (2026-05-05)

## Idea

Gate Calibrate tool second measurement pick so it only accepts geometry whose **resolved face** is compatible with the first pick, using the same parallelism rule as nominal span (`CalibNominal::SpanBetweenFaces`): `|dot(na, nb)| >= 0.75`, distinct faces. This filters hover and click commit.

## Plan

- Centralize threshold as `CalibrateNominal::kFaceNormalParallelAlignThreshold` and add `NormalsAlignedForCalibPick`.
- Helper `CalibSecondPickAcceptsHit` in `display.cpp` (anonymous namespace) using existing `ResolveCalibFaceForWorkflow`.
- `UpdatePickHover` / `TryCommitCalibrateFacePick`: when point 2 row is active and point 1 has a pick, reject non-matching hits.

## Note on “same normal”

Product language “same normal” often means **coplanar parallel faces** (including **opposite** outward normals on opposite sides of a slab). This implementation allows **parallel or opposite** so box thickness still works; it does **not** require `dot > 0` only.

## Outcome

- Clean build of `CAD_OpenGL`.
- Second prerequisite subtitle updated to “parallel to first pick”.

## Retro

- `CalibPickHit` is private; passing `(hitFace, hitEdge)` to a file-local helper avoided exposing the nested type.
