# Geometry validity (`AppInvalidTag`) — mitigations and repair roadmap

This note captures how we think about **invalid app geometry**, what to tell users, and how we might **move toward valid** geometry over time. It complements the code in `include/GeometryValidity.hpp`, `src/logic/GeometryValidity.cpp`, and the **per-solid cache** on `Solid` (`cachedAppInvalidGeometryTags`, `cachedAppInvalidGeometryTagsFresh`).

## Principles

1. **Validity is contextual** — The same mesh can be fine for viewing but invalid for an operation that needs a closed solid, consistent orientation, or CGAL soup guarantees. Prefer wording like “invalid **for this step**” in product copy when appropriate.

2. **Trust the cache only when fresh** — Before using `Solid::cachedAppInvalidGeometryTags` for gating or UI, ensure `cachedAppInvalidGeometryTagsFresh` is true, or call `GeometryValidity::RefreshSolidAppGeometryValidityCache` (lazy refresh on read is an acceptable safety net if some mutation path forgot to invalidate).

3. **Structure-specific pick rules** stay out of this table for now — Planar-only, single-loop, “upward enough”, and min-span rules live in `Display::IsStructureFaceEligible` until that pipeline stabilizes; see `documentation/implementations/structure_face_triangulation_2026-05-11.md`.

4. **CGAL has its own vocabulary** — Soup/mesh outcomes use `CgalPolygonSoupTag` and CGAL’s definitions; a different kernel would need a parallel enum.

## `AppInvalidTag` reference (detection today vs future)

| Tag | What it means (app layer) | Detected today? | Default user-facing angle | Mitigation / repair ideas (phased) | When to refuse vs warn |
|-----|---------------------------|-----------------|----------------------------|-----------------------------------|-------------------------|
| `DegenerateTriangle` | Near–zero area triangles from fan decomposition of face loops (scale-aware threshold). | Yes | “Some faces are too thin or collapsed.” | **Shipped (2026-05-14):** `TryRepairDegenerateSolidBRep` — spatial-hash weld of straight-edge endpoints (skips curved / bridge edges), then removes faces still failing the same fan area test. Further: edge-collapse / remesh if needed. | **Refuse** steps that need stable facet areas; **warn** for display-only. |
| `NullOrEmptyTopology` | Missing solid/face/surface, empty loops, null edge/points, or fewer than three vertices in a loop after a clean walk. | Yes | “The model’s connectivity is broken or incomplete.” | **Usually a bug or aborted op** — fix the pipeline; optional recovery from last good snapshot. | **Refuse** most geometry ops until fixed. |
| `SelfIntersection` | Surface passes through itself (volumes ambiguous). | **Partial (2026-05-14):** planar **face** boundary loops only — proper 2D self-crossings and crossings between loops on the same planar face (projected to the plane). Non-planar / inter-face 3D pierce not covered yet. | “The model intersects itself.” | **Later:** triangle-soup tests, boolean cleanup, or user-guided repair. | **Refuse** robust booleans / carve until addressed or user accepts risk. |
| `OpenBoundary` | At least one `Edge` appears on only one face in the solid’s half-edge count (sheet or open shell). | Yes | “This solid has a boundary — it is not a closed volume.” | **Design (2026-05-14):** phased import / shared diagnostics + tool gating; **no** undefined “patch” toggle until a named repair exists — see `documentation/implementations/import_open_boundary_ux_2026-05-14.md`. **Later:** optional hole fill / cap behind explicit user action. | **Warn** by default; **refuse** only for tools that require watertight input. |
| `NonManifoldConnectivity` | An edge shared by more than two face sides, or two uses that do not oppose along the same endpoints. | Yes | “Edges meet in a way this operation cannot handle.” | **Plan (2026-05-14):** detect → UI message → gate CGAL-sensitive tools; explicit repair + preview later — `documentation/implementations/non_manifold_connectivity_next_2026-05-14.md`. CGAL mesh path may already use PMP helpers; B-rep repair is separate. | **Refuse** CGAL soup paths that need manifold-like input; **warn** otherwise. |
| `InconsistentFaceOrientation` | Same directed edge used twice from adjacent faces (normals / winding disagree). | Yes | “Face directions disagree on shared edges.” | **Shipped (2026-05-14):** `TryRepairInconsistentFaceOrientationSolid` + `Face::FlipWindingIfPlanar` for **two-face manifold** planar edges (iterative). **Later:** NURBS, multi-shell volume sign, non-manifold. | **Refuse** volume-dependent ops; **warn** for rendering. |

## Suggested implementation order (engineering)

1. **UX and gating** — Map tags (and CGAL failures) to short, actionable strings; block only when the **current tool’s contract** requires it.
2. **Low-ambiguity fixes** — Vertex merge / duplicate weld (already partially present on STL import), drop **zero-area** facets where safe.
3. **Explicit repair commands** — Non-manifold repair, orientation, hole fill — **with preview** and clear “cannot auto-fix” paths.
4. **Self-intersection** — **v1 detection shipped (2026-05-14)** for planar face loops; extend with mesh-level tests before heavy auto-repair.

## Code touchpoints (for maintainers)

- Evaluation: `GeometryValidity::EvaluateAppInvalidTagsForSolid`
- Cache: `Solid::cachedAppInvalidGeometryTags`, `Solid::cachedAppInvalidGeometryTagsFresh`
- Refresh: `GeometryValidity::RefreshSolidAppGeometryValidityCache` — after `Scene::CreateSolid`, end of `Scene::MergeCoplanarFaces`
- Invalidate: `GeometryValidity::InvalidateSolidAppGeometryValidityCache` — before tearing down a solid’s faces (e.g. Structure carve / CGAL STL experiment detach)
- Carve diagnostics: `StructureCarve` logs app tags on certain failures (see `STRUCT_MESH_*` handling in `display.cpp` for user-visible errors)

## Revision

- **2026-05-14** — Initial table and principles.
- **2026-05-14** — OpenBoundary import/UX design + NonManifold next-case plan: `import_open_boundary_ux_2026-05-14.md`, `non_manifold_connectivity_next_2026-05-14.md`.
