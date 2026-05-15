# Self-intersection — planar loop detector (v1) (2026-05-14)

## Goal

Set `GeometryValidity::AppInvalidTag::SelfIntersection` from **`EvaluateAppInvalidTagsForSolid`** for cases we can classify cheaply and deterministically before mesh-level or repair work.

## Approach (v1)

- **Scope:** **Planar** `Face` instances only (`surface->IsPlanar()`). **NURBS / non-planar** faces do not contribute from this pass (flag may still be clear until a future triangle-soup or surface test exists).
- **Method:** For each face, collect each boundary loop as ordered 3D vertex positions (`OrientedEdge::GetStart`). Project to the face plane with an orthonormal basis from the unit normal and a loop origin. Use **proper** 2D segment–segment intersection (strict crossing, scale-aware cross-product epsilon from max edge length on that face).
- **Checks:** (1) each loop against itself (non-adjacent edges), (2) every **pair** of distinct loops on the same face (outer vs hole, hole vs hole).
- **Integration:** Called at the end of `EvaluateAppInvalidTagsForSolid` after edge/vertex connectivity tagging.

## Limitations

- Does not detect **3D** sheet–sheet intersection between **different** faces (non-coplanar).
- Touches / overlaps without a **proper** segment crossing may be missed (by design of proper intersection).
- Epsilon heuristics may need tuning if false positives appear on large coordinates.

## Outcome

Implemented in `src/logic/GeometryValidity.cpp`; public comment updated in `include/GeometryValidity.hpp`. Build: `cmake --build build --target CAD_OpenGL`.

## Mini retro

- GLM 1.0 in this tree has no `glm::length2` for `dvec2`; used `glm::dot(v, v)` for squared length.
