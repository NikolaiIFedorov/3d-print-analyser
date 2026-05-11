# Structure tool — face triangulation (2026-05-11)

## Idea

Replace the retired diamond / inset / center-strut infill preview (see `structure_tool_center_struts_preview_2026-05-10.md`) with a **face triangulation** tool: the user picks an upward-facing planar face on a solid, and the tool carves a *printable vertical void* out of the part whose footprint is a *triangulated inset pattern* derived from the face's outline. Each carved feature is a vertical prism (walls parallel to `+ẑ`, opening to the top of the part), so it prints without overhangs, bridges, or supports.

Positioning: this is a **structural-intent tool** for parts that would otherwise be solid — *not* a competitor to slicer infill on hollow shells. Use cases include lightweighting solid brackets, exposing aesthetic internal structure, and pre-planning weight reduction in CAD before slicing.

## Algorithm (per face)

Notation: build-up direction = world `+ẑ`. Face outward normal = `n`. Inset distance = `i` (mm). Min feature size = `f_min` (mm).

For a user-selected face whose outward normal satisfies `n·ẑ ≥ kMinUpComponent` (default `0.3`; vertical and downward-facing faces ineligible because the XY projection of a vertical face collapses to a line):

1. **Polyline + project.** Walk the face's outer B-rep loop, polyline any arc / curve edges into short segments under a chord-tolerance. Project every vertex to XY by dropping its Z. Maintain a parallel boolean vector marking which polygon vertices correspond to **original B-rep vertices** (a.k.a. *corner vertices*) — polyline-introduced ones are *not* corners.

2. **Inset.** Apply CGAL's straight-skeleton interior offset (`CGAL::create_interior_straight_skeleton_2` + offset polygons) by `i`. The output is a polygon (or polygon-with-holes if the inset disconnects), called the **inset polygon**. Track corner-vertex identity through the offset by projecting each input corner inward along its angle bisector by `i` and snapping to the nearest offset-polygon vertex.

3. **Suppress strips on too-small holes.** If the inset polygon's bbox span < `f_min`, or it has fewer than 4 inset corner vertices, **emit only the inset ring + outer ring** — skip steps 4 and 5.

4. **Strip.** For an inset polygon with exactly 4 inset corner vertices, build the candidate non-adjacent chord set (the 2 diagonals). Default rule: **shortest chord that lies entirely inside the inset hole**. Build the strip as a thin band along that chord, width = `i`. For ≥ 5 corner vertices, v1 emits **one strip** by the same rule and ignores the rest. (Multi-strip → Phase D.)

5. **Fillet.** Round the inner corners (strip-meets-ring, plus any G⁰ break on the inset polygon itself) with radius `i` — fillet radius is tied 1:1 to the inset distance, so the panel exposes one parameter, not two. Implementation fallback if CGAL has no direct 2D fillet: *offset-out by r, then offset-in by r* on the unioned polygon, which produces fillets at concave vertices as a side effect.

6. **Extrude upward.** Triangulate the resulting 2D polygon-with-holes (CGAL `Constrained_triangulation_2`) and extrude along `+ẑ` from `min(face.Z) − ε` up to `solid.AABB.max.z + ε`. The bottom cap of the prism must match the **face's actual 3D geometry**, not the flat XY footprint — for slanted faces the prism floor is slanted.

7. **Boolean subtract.** `CGAL::Polygon_mesh_processing::corefine_and_compute_difference(solid, prism, solid)` to remove the prism from the owning solid.

Resulting void: vertical walls, open to the top → trivially printable.

## Plan

Phased; v1 stops at preview (no boolean), v2 adds the carve.

### Phase A — scaffolding cleanup (no behavior change)

- Append the **2026-05-11 — Decision: retire** entry to `structure_tool_center_struts_preview_2026-05-10.md`. *(Done as the contract for this pivot.)*
- Remove `BuildAdjacentFaceMidpoints`, `BuildInteriorFaceRibs`, `BuildInsetFaceLoops`, `BuildCenterStruts`, and the `PreviewPattern` enum from `StructurePreview.{hpp,cpp}`. Keep the anonymous-namespace helpers (`SolidBounds`, `FaceCentroidPlanar`, `OutwardNormalPlanar`, `RayAabbInterval`, polygon-clip helpers, `AppendRibRectangle`) — they will be reused.
- Decide whether to rename `StructurePreview` → `StructureTriangulation` or keep the namespace generic. Bikeshed during implementation.
- Strip diamond/inset-specific fields and refresh calls from `Display`: `RefreshStructurePreviewForRenderer` clears its rib buffer use; `structureRibSegments` / `structurePreviewEnabled` (and friends) removed.
- Structure toolbar entry stays. Tool panel still renders (subtitle + import prerequisite + cancel/accept footer); parameter-empty until Phase B sliders land.
- Update panel subtitle from "Save on printed weight with specialized infill" → e.g. *"Carve printable patterns into solid faces"* (final copy TBD).

### Phase B — algorithm, preview only

- **Face selection mode.** Single-face selection on the Structure tool: hover-highlight, click-to-select. Reuse Calibrate's hover/highlight machinery if face-level picking exists; otherwise extend picking (see Risks).
- **Eligibility gate.** Reject faces with `n·ẑ < kMinUpComponent`, non-planar faces, faces with holes (`loops.size() > 1`), and faces whose bbox span < some lower bound. Panel message identifies why a rejected face was skipped.
- **B-rep → polygon.** Walk the selected face's outer loop, polyline non-line edges with a configurable chord tolerance (default `0.1` mm), build a CGAL `Polygon_2`. Maintain corner-vertex tags.
- **Inset via straight skeleton.** Panel slider `insetMm` (default `2.0`). Generate the inset polygon. Match corner vertices (see Risks for the matching strategy).
- **Corner count + strip suppression.** Algorithm step 3.
- **Strip.** Algorithm step 4 (shortest non-adjacent chord, thin band, polygon-union with inset ring).
- **Fillet.** Algorithm step 5 (offset-out / offset-in fallback if needed).
- **Preview render.** Convert the resulting 2D polygon-with-holes into 3D line segments at the face's actual plane (so the preview sits on the face, not the XY projection), and feed them into the existing `structurePreviewSegments` buffer.
- **Visual verification** on:
  - axis-aligned rectangle (top face of a 30 mm cube),
  - pill on a top face,
  - rectangle on a slanted face (projection case),
  - pill on a slanted face (projection + curve case),
  - small face (strip-suppression case),
  - face with a re-entrant outer loop (graceful-failure case).

### Phase C — boolean carving, v2

- **2D triangulation.** `CGAL::Constrained_triangulation_2` of the final polygon-with-holes.
- **Vertical extrusion.** Build a closed prism mesh from `min(face.Z) − ε` to `solid.AABB.max.z + ε`. Bottom cap = face's actual 3D geometry (slanted-face support).
- **3D boolean.** `corefine_and_compute_difference(solid, prism, solid)`. On failure (non-manifold input, near-coplanar coincidences), report and skip; never half-carve.
- **Commit on Accept.** `FinalizeStructureSceneToolSession(accepted=true)` replaces the original solid mesh with the carved one. Cancel discards. Mark scene faces dirty so downstream renderers re-pack.

### Phase D — follow-ups (explicitly out of v1)

- Multi-face triangulation in one tool session.
- User override of corner pairing (interactive chord pick / numeric input).
- Multi-strip on ≥ 5 corner-vertex faces.
- Curve-aware offset (drop the polylining step; use a curve-capable offset library or CGAL package).
- Downward-facing face support with a "carve downward into the part" mode.
- Smart strip selection based on the face's principal axis, if the shortest-chord default proves wrong in practice.

## Definitions (locked for v1)

- **Corner vertex** = a B-rep vertex on the face's outer loop. *Topological*, not geometric — Plasticity (and our modeller) place vertices at line-to-arc / arc-to-arc / line-to-line junctions, so the 4 purple points on a pill in `image-2a9fa420-…png` are corner vertices regardless of tangent continuity.
- **Up direction** = world `+ẑ`. Reuses the `kBuildUpWorldZ` convention from the prior `StructurePreview.cpp`.
- **Inset (`i`)** = panel parameter, default `2.0` mm. Drives skeleton offset, strip band width, and fillet radius (1:1 with `i`). Single source of truth for all three; no separate fillet slider.
- **Min feature size (`f_min`)** = panel parameter, default `1.5` mm. Drives small-hole strip suppression and small-face eligibility.
- **`kMinUpComponent`** = `0.3`, compile-time constant for v1. Anything more vertical is rejected.

## Risks / open questions

- **CGAL straight-skeleton vertex correspondence.** Assuming we can match input corners to offset-polygon vertices reliably. If skeleton events collapse edges or merge vertices, naive 1:1 mapping breaks. Fallback: project each input corner inward along its angle bisector by `i` and snap to the nearest offset-polygon vertex within `f_min`. Verify on Phase B visual-test cases before locking.
- **2D fillet implementation.** CGAL doesn't ship one. Offset-out / offset-in via straight skeleton works but approximates the arc with a polyline. Acceptable for preview; revisit if carved geometry needs analytic arcs.
- **Mesh-boolean robustness (Phase C).** `corefine_and_compute_difference` is sensitive to coplanar input. Apply a small `ε` perturbation to the prism's bottom Z; consider switching to `Exact_predicates_exact_constructions_kernel` for the boolean step even if the rest of the codebase runs an inexact kernel.
- **Face-level picking.** Per `Architecture_Input.md` current picking is focused on edges and vertices. Selecting a face needs either a per-face hit-test or a "click an edge of the face you want" indirection. Phase B may need a small picking extension.
- **Slanted-face prism floor.** The prism's bottom isn't flat — it follows the slanted face. Phase C step "Vertical extrusion" has to build the bottom cap as a copy of the original face geometry rather than as a flat polygon at `min(face.Z)`. Worth confirming with a slanted-face test before treating it as solved.

## Outcome

### Phase A — scaffolding cleanup (2026-05-11)

**Removed.** `BuildAdjacentFaceMidpoints`, `BuildInteriorFaceRibs`, `BuildInsetFaceLoops`, `BuildCenterStruts`, the `PreviewPattern` enum, and `RibPreviewParams` from `src/logic/Structure/StructurePreview.{hpp,cpp}`. From `Display`: `structureInsetFaceMm` / `structureInsetFaceDepthMm` / `structureInsetFaceFullDepthThroughSolid`, and the diamond/inset call paths inside `RefreshStructurePreviewForRenderer`. From `SceneRenderer`: `SetStructureRibSegments`, `SetStructureInsetFaceSegments`, the matching private buffers, and the rib/inset color appends in `CommitStructurePreviewLinesToGpu`. Panel subtitle changed from "Save on printed weight with specialized infill" → "Carve printable patterns into solid faces".

**Kept.** All anonymous-namespace helpers in `StructurePreview.cpp` (`ExpandBounds`, `SolidBounds`, `FaceCentroidPlanar`, `OutwardNormalPlanar`, `MergeCloseSorted`, `ClipConvexPolygonVerticalLine`, `ClipConvexPolygonHorizontalLine`, `ApplyChordEndInset`, `ChordTooHorizontalForBuildUp`, `RayAabbInterval`, `AppendRibRectangle`), each annotated `[[maybe_unused]]` so they pass `-Wunused-function` until Phase B re-introduces callers. Structure toolbar entry, panel scaffolding (subtitle + import prerequisite + cancel/accept footer), translucent shell toggle (`structureTranslucentShellEnabled`), and the `SceneRenderer::structurePreviewSegments` main preview buffer all stay.

**Tool state during Phase A.** Selecting the Structure tool brings up the panel with its new subtitle, the import prerequisite, the translucent shell, and the cancel/accept footer; `RefreshStructurePreviewForRenderer` clears the preview buffer so no preview lines render. The tool is intentionally dormant until Phase B lands.

**Build.** `cmake --build build --target CAD_OpenGL` succeeded with no warnings introduced.

**Namespace.** `StructurePreview` retained for Phase A — renaming to `StructureTriangulation` (or similar) is deferred to Phase B per the bikeshed-during-implementation note in the Plan.

## Mini retro — Phase A

- The pivot deletion was small because the prior architecture already separated "Structure tool scaffolding in `Display` / `SceneRenderer`" from "infill generators in `StructurePreview`." Only one file (`StructurePreview.cpp`) lost real logic; the rest was field/method housekeeping in three other TUs. Worth remembering as evidence that the Structure-tool layering held up under a feature swap.
- `[[maybe_unused]]` on the retained helpers avoids the alternative of speculatively re-deleting and re-typing them in Phase B. Costs one attribute per declaration; reads as a deliberate "kept for Phase B" signal. Will revisit during Phase B and remove the annotations as callers reappear.
- One snag worth a one-line proposed `best_practices.md` Stage-3 addition: *"When carving out a feature, grep the symbol set across both the implementation TU and every consumer; never delete only the definition."* The `structureInsetFaceMm` / `structureInsetFaceDepthMm` fields lived in `display.hpp` for a removed sliders panel and would have lingered as dead state if we had only edited `StructurePreview.{hpp,cpp}`.
