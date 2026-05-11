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

**Tool state during Phase A.** Selecting the Structure tool brings up the panel with its new subtitle, the import prerequisite, and the cancel/accept footer; `RefreshStructurePreviewForRenderer` clears the preview buffer so no preview lines render. The tool is intentionally dormant until Phase B lands.

**Follow-up edit (same session, 2026-05-11): translucent shell removed from Structure tool.** The diamond/inset preview needed x-ray rendering so users could see *inside* the solid; the face-triangulation overlay will live on the *outside* of the picked face, so normal rendering is correct. Removed `Display::structureTranslucentShellEnabled`, the local `structureShellTranslucent` gate, the early grid draw, the `SetStructureViewTranslucentSolid` call, the `RenderSolidWireframeOccludedOverlay` call, the deferred grid branch, and the `RenderAxesBehindScene` call. The underlying infrastructure (`SceneRenderer::SetStructureViewTranslucentSolid` / `RenderSolidWireframeOccludedOverlay`, `OpenGLRenderer::DrawSolidWireframePrefixBehindDepth` + split-shell path + the `uStructureShellBackFaceOpaque` shader uniform, `ViewportRenderer::RenderAxesBehindScene`) is now dormant but still present. **Follow-up to file separately**: rip out that dormant infrastructure; it crosses module boundaries (Display / SceneRenderer / OpenGLRenderer / ViewportRenderer / `basic.frag`) and trips the workspace scope-gate, so it deserves its own task with the recommended routing rather than riding along with Phase A.

**Build.** `cmake --build build --target CAD_OpenGL` succeeded with no warnings introduced.

**Namespace.** `StructurePreview` retained for Phase A — renaming to `StructureTriangulation` (or similar) is deferred to Phase B per the bikeshed-during-implementation note in the Plan.

### Phase B1 — face picking + eligibility gate (2026-05-11)

**Scope.** Single sub-phase of Phase B, kept narrow to land in one session: hover-highlight + click-to-select for a single face on the Structure tool, plus a coarse eligibility gate. No CGAL, no inset, no preview geometry beyond the existing face-highlight fill — those land in B2.

**Reused, not duplicated.** Existing pick infrastructure already supported faces: `ScenePick::PickClosestFace` + `PickFilter::Faces` + `SceneRenderer::GetPickTriangles` cover ray hit-tests, and `Display::RebuildPickHighlightMesh` already renders hovered/selected face fills via accent gradient (eligible) or solid gray (rejected). Calibrate was the only consumer; B1 extends the same paths instead of cloning them.

**Renamed.** `Display::SetHoverCalibPick` → `SetHoverPick`, `hoverCalibPickRejected` → `hoverPickRejected`. Both are now tool-agnostic — the rejection-render branch in `RebuildPickHighlightMesh` no longer reads as Calibrate-specific. Calibrate behaviour is unchanged; the only difference is that the field/method now mean "the active tool's hover failed *its* eligibility gate," with the gate itself living in each tool's pick method.

**Added state.** `Display::structureSelectedFace` (single-slot committed pick), `Display::structureHoverIneligibleReason` (panel hint string). The pick-dirty gate (`pickDirty || hoverPickFace != nullptr || …`) now also tracks `structureSelectedFace`, and the stale-face cleanup at the top of the render path (`stillPickable(...)`) clears the structure slot when the scene reload drops the face.

**Added methods.**
- `Display::PickStructureAtPixel(pixelX, pixelY)` — wraps `OrthoPickRay` + `PickClosestFace`, returns `StructurePickHit { face, eligible, ineligibleReason }`.
- `Display::IsStructureFaceEligible(face, &reason)` — gate: non-null, planar (`Surface::IsPlanar()`), `loops.size() == 1`, `surface->GetNormal().z >= 0.3` (constant `kStructureMinUpComponent`), and per-face AABB max-extent `>= 1.5 mm` (constant `kStructureMinFaceSpanMm`, computed via a file-local helper that walks the outer loop's oriented-edge start positions). Every failure path writes a human-readable reason into `*outReason`.
- `Display::TryCommitStructureFacePick(pixelX, pixelY)` — mirror of `TryCommitCalibrateFacePick`; bails on UI/ImGui/`activeTool` mismatch, refreshes the hint on ineligible click, commits the face slot + calls `RefreshStructurePreviewForRenderer` on eligible click. Wired from `Input.cpp` alongside the Calibrate commit (each method checks `activeTool` and returns when the tool doesn't match, so calling both is safe).
- `Display::ClearStructureFacePick()` — called from the Structure toolbar toggle and from the `pendingToolSwitch` handler in `Display::Render`.

**Extended methods.**
- `GetActivePickFilter()` returns `Faces` for Calibrate (existing logic) and for Structure (new branch, gated on import-prerequisite `calibStepImport == Done`).
- `UpdatePickHover()` now branches on `activeTool`: Structure path calls `PickStructureAtPixel` + `SetHoverPick(face, nullptr, !eligible)` + updates the panel hint; Calibrate path is unchanged. Both shared early-outs (ImGui capture, viewport-nav drags, `PickFilter::None`) now also clear the structure hover reason via a small `clearStructureHoverReason` lambda.
- `ClearPickHover()` resets the structure hover reason in addition to the existing hover face/edge fields.
- `SetHoverPick()` early-out also tracks `structureSelectedFace` so we don't suppress a dirty mark when the structure slot is set.
- `RebuildPickHighlightMesh()` appends `structureSelectedFace` triangles with the same accent tint as Calibrate's committed picks, and the hover-suppression line also bails when the hover face equals `structureSelectedFace`.

**Panel hint.** New `structPara_HoverHint` paragraph appended below the scene-edit footer. `SyncStructurePanelDerivedVisibility` toggles its visibility (`importDone && !reason.empty()`) and copies the reason string into `values[0].text`. Hover/click paths call this helper whenever the reason changes, so the hint updates live without an explicit per-frame poll.

**Build.** `cmake --build build -- -j8` succeeded clean; no new warnings; `ReadLints` on the three edited TUs reported zero diagnostics.

**Deferred to B2.** CGAL straight-skeleton offset, corner-vertex tagging, strip + fillet, preview-line buffer fill. The hint copy is intentionally generic (no face id, no normal angle) until B2 has enough context to be more specific.

### Phase B1.5 — opt-out UX (2026-05-11)

**Why.** Single-face opt-in left the tool invisible on entry — the user had to click before anything happened. Inverted the model: every eligible face is triangulated by default; clicking *excludes* it. The user proposed this; performance scaling with model complexity is accepted (bake cost is expected, toggle cost must not be — that's a B2 cache concern).

**State change.** `Display::structureSelectedFace` (single `const Face *`) → `Display::structureExcludedFaces` (`std::unordered_set<const Face *>`). Added `Display::structureEligibleFacesCache` (also a set) — rebuilt inside `RebuildPickHighlightMesh` when the Structure tool is active, so the render loop can apply the included-by-default tint without re-running the eligibility gate per triangle.

**Click semantics.**
- Eligible face → toggle membership in `structureExcludedFaces` (add if absent, remove if present).
- Ineligible face → unchanged: refresh the hint string, no exclusion-set mutation.
- Empty space / no hit → no-op.

**Render change.** `RebuildPickHighlightMesh` now appends every face in `structureEligibleFacesCache \ structureExcludedFaces` with a faint accent (`appendFaceTris(f, 0.35f, 0.45f)`), skipping the hover face so the existing hover branch can paint it brighter (consistent interactive feedback). Removed the old `hoverDraw == structureSelectedFace` suppression — no longer relevant.

**Dirty-flag gate.** The early-return at the top of `RunPickNode` now treats `activeTool == ActiveTool::Structure` as always dirty (even if no exclusion and no hover), so the included tint shows up on tool entry without waiting for a stray pick event.

**Stale-face cleanup.** Replaced the single-pointer null-check with a loop that erases stale pointers from `structureExcludedFaces`. The set survives scene tab switches by accident only — the per-frame stale cleanup removes old-scene pointers as soon as the new scene's pick triangles come in. Considered explicit clearing on tab switch / post-import; deferred until it causes a visible bug.

**Panel hint.** `SyncStructurePanelDerivedVisibility` now keeps the hint paragraph visible whenever the import prerequisite is satisfied, and falls back to the default instructional copy `"Click an eligible face to exclude it from triangulation."` when no rejection reason is set. Rejection reasons still take priority on ineligible hover.

**Build.** `cmake --build build -- -j8` succeeded clean; `ReadLints` clean on `display.{hpp,cpp}`.

**Cache caveat for B2.** `structureEligibleFacesCache` is currently rebuilt every time `RebuildPickHighlightMesh` runs (i.e. roughly per hover update). For B1.5 the cost is trivial — set inserts and a planarity check per face. For B2 the bake output **cannot** ride along with this cache: it has to be keyed by face pointer + inset value + fillet value + chord tolerance and invalidated only when those change. Toggling exclusion must never invalidate the per-face geometry; it should only flip the inclusion bit and re-emit preview lines.

### Phase B2a — CGAL scaffold + module rename (2026-05-11)

**Build dependency change.** `CAD_EXPERIMENTAL_CGAL_PLANAR_REMESH` (default OFF) → `CAD_USE_CGAL` (default ON). The preprocessor define follows suit: `CAD_CGAL_PLANAR_REMESH_EXPERIMENT_ENABLED` → `CAD_USE_CGAL`. Both consumers (the legacy STL planar-remesh experiment and the new face triangulation) now share the same flag. The option is preserved (not hard-required) so distributions that need to dodge the (L)GPL transitive licence can disable it; with the flag off, the Structure tool will surface a "CGAL not available" hint and the bake returns no segments (the latter is the Phase A behaviour, just gated on the flag instead of the missing algorithm).

**Module rename.** `src/logic/Structure/StructurePreview.{hpp,cpp}` deleted; replaced by `src/logic/Structure/StructureTriangulation.{hpp,cpp}`. The Phase-A `[[maybe_unused]]` helpers retired with the file — none survived the eligibility-gate detour (the gate uses a single file-local helper in `display.cpp` and direct virtual calls on `Surface`/`Face`). Render-side type names (`SetStructurePreviewSegments`, `RenderStructurePreviewLines`, `CommitStructurePreviewLinesToGpu`) stay as-is — they describe the *preview line buffer*, not the retired generator namespace, and remain accurate.

**Module surface.**
- `BakeParams { insetMm = 2.0, chordTolMm = 0.1, minFeatureMm = 1.5 }` — the cache key. Equal params for the same face returns cached output.
- `BuildFaceTriangulationPreview(face, params)` — stub returns empty for B2a; B2b–e replace the body. Cache hit/miss path is already in place so future commits don't have to rewire the contract.
- `ClearBakeCache()` and `InvalidateBakeCacheForParams(params)` — invalidation hooks. Wired by `Display` in B2f when the slider drag lands.

**Display wiring.** Added `Display::structureInsetMm` (`float`, default `2.0`) as the single source of truth for the panel slider value. `Display::RefreshStructurePreviewForRenderer` now iterates `structureEligibleFacesCache \ structureExcludedFaces` and concatenates each face's `BuildFaceTriangulationPreview` output into the segment buffer handed to `SceneRenderer::SetStructurePreviewSegments`. Today every face returns an empty list, so the buffer is still empty — but the data path is live and B2b just needs to fill in the algorithm.

**Slider UI deferred.** The panel knob for `structureInsetMm` waits for B2b — once the first geometry is visible, the slider has something to react to. For B2a the field is set to the spec default and not exposed.

**Build.** `cmake -B build -DCAD_USE_CGAL=ON` (clean reconfigure) + `cmake --build build -- -j8` succeeded with no warnings; the new TU `StructureTriangulation.cpp.o` builds at `[98%]`. `ReadLints` clean across the new files, `display.{hpp,cpp}`, and `CMakeLists.txt`.

**Next.** B2b: walk the selected face's outer loop, polyline NURBS / arc edges adaptively against `chordTolMm`, project to the face's plane (so slanted faces work), build a CGAL `Polygon_2`, and emit the projected outer loop as preview lines. That's the first visible result — a coloured outline tracing each included face. Corner-vertex tagging starts here so B2c's offset can carry the labels through.

## Mini retro — Phase A

- The pivot deletion was small because the prior architecture already separated "Structure tool scaffolding in `Display` / `SceneRenderer`" from "infill generators in `StructurePreview`." Only one file (`StructurePreview.cpp`) lost real logic; the rest was field/method housekeeping in three other TUs. Worth remembering as evidence that the Structure-tool layering held up under a feature swap.
- `[[maybe_unused]]` on the retained helpers avoids the alternative of speculatively re-deleting and re-typing them in Phase B. Costs one attribute per declaration; reads as a deliberate "kept for Phase B" signal. Will revisit during Phase B and remove the annotations as callers reappear.
- One snag worth a one-line proposed `best_practices.md` Stage-3 addition: *"When carving out a feature, grep the symbol set across both the implementation TU and every consumer; never delete only the definition."* The `structureInsetFaceMm` / `structureInsetFaceDepthMm` fields lived in `display.hpp` for a removed sliders panel and would have lingered as dead state if we had only edited `StructurePreview.{hpp,cpp}`.
