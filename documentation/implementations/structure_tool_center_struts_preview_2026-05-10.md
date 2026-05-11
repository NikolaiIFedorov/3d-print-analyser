# Structure tool — center struts preview (2026-05-10)

## Idea

Add a third toolbar tool **Structure** (alongside Analysis and Calibrate) for adaptive internal structure. v1 ships **preview only**: line segments from each planar face centroid toward the solid interior (bbox center heuristic), clamped in length, drawn in the loose wireframe pass. **Inner offset walls** are UI-only “coming soon” (no geometry yet).

## Plan

1. `StructurePreview::BuildCenterStruts` under `src/logic/Structure/` — bbox per solid, face centroids, strut length capped by fraction of distance to center and fraction of bbox diagonal.
2. `SceneRenderer` holds preview segments; `RebuildLoose` appends them after `GenerateLoose`.
3. `Display`: `ActiveTool::Structure`, toolbar icon, `BuildToolPanel`-style panel with ImGui checkboxes, `RefreshStructurePreviewForRenderer()` before geometry rebuilds, `pendingToolSwitch` / `SyncToolbarToolVisualState` / session ordinal updates.

## Outcome

- **Shipped:** Third toolbar tool **Structure** with panel (subtitle + two rows: live checkbox for center-strut preview, disabled row for inner offset walls). `StructurePreview::BuildCenterStruts` walks each solid’s planar faces, builds short segments from face centroid toward the solid’s bbox center (length capped), and `SceneRenderer` appends them in `RebuildLoose` with accent-colored lines.
- **Build:** `cmake --build` (macOS, AppleClang) succeeds.
- **Follow-ups:** Ray-based depth to opposite wall instead of bbox center; offset-shell mesh / boolean apply; tie into `RebuildSolids`-only path if partial solid updates become common; throttle preview for very large face counts.

## Mini retro

- Reused `BuildToolPanel` + `SceneRenderer::RebuildLoose` extension to avoid a parallel overlay upload path; keeps one wire mesh path at the cost of recomputing segments whenever full solid geometry rebuild runs.
- `pendingToolSwitch` needed explicit `RefreshStructurePreviewForRenderer` + `MarkGeometryDirtyAll` when `analysisEnabled` unchanged (e.g. Calibrate → Structure) so preview lines appear on first switch.

---

## Update: translucent solid shell in Structure view (same log)

- **UI:** Checkbox “Translucent solid shell (see inside)” (default on) in Structure panel.
- **Render:** `UploadAllPacked` records `packedSolidPatchIndexCount` (triangle indices belonging to solid chunks before loose patch). `OpenGLRenderer::DrawTriangles` draws that prefix in two sub-passes: depth-only (write Z), then blended color with `uAlpha` and **no** depth write, so wireframe struts still depth-test against the shell. Remaining indices (loose patch) draw opaque as before.
- **Note:** Split path is skipped when `kDepthPrepassOpaquePatches` is enabled (experiment flag; default off).
- **Fix:** Original split gate required `solidPrefix < triangleIndexCount`, so solid-only meshes (empty loose patch) never went translucent — allow `solidPrefix <= triangleIndexCount`.

---

## Update: translucency reads dark + no grid/struts behind (2026-05-10)

**Causes:** (1) Blending ran before the grid and against the clear color; stencil still hid the late grid under the shell. (2) Shell depth prepass occluded interior strut segments in the main wire draw.

**Changes:** Draw the reference grid once **before** patches when Structure translucent shell is on (stencil off), and **skip** the second `viewportRenderer.Render()` for that mode. Upload center-strut segments to a **separate** line mesh and draw **twice** (normal depth + `DepthCompareBehind` x-ray) like pick span overlay. Slightly higher shell alpha (0.42) in `Display::Render`.

---

## Reference axes occluded pass (Structure translucent shell)

RGB axes drew with normal depth only, so segments behind the shell depth failed. Added `ViewportRenderer::RenderAxesBehindScene()` (`DepthCompareBehind` + blend), called after `RenderAxes()` when Structure translucent shell is active.

**Solid wireframe back edges:** Track `packedSolidWireframeIndexCount` in `RepackOffsets` (line index prefix before loose wire). `OpenGLRenderer::DrawSolidWireframePrefixBehindDepth` draws only that prefix with `DepthCompareBehind` when Structure translucent shell is on (after main `RenderWireframe`).

**Fix (edges still invisible):** Behind-pass used lit wireframe colours; grazing / back-lit normals yielded near-black fragments at ~0.42 α on a dark viewport. Switched occluded-solid-wire pass to **unlit** vertex colour (`uLightingEnabled = 0`) and aligned α (~0.52) with axes behind-pass.

---

## Update: adjacent-face midpoints preview (3D diamond / octahedron on cube) — 2026-05-10

**Plan:** Prefer the face-adjacency graph (centroid-to-centroid across each manifold edge shared by two planar faces of the same solid) as the default preview; keep bbox center struts as a legacy combo option. Ribs / filleted solids deferred.

**Code:** `StructurePreview::BuildAdjacentFaceMidpoints`, `PreviewPattern` enum. `Display`: “Structure preview lines” checkbox + “Preview pattern” combo; default pattern `AdjacentFaceMidpoints`. Renamed `structureCenterStrutsEnabled` → `structurePreviewEnabled`.

**Behaviour:** Skips edges unless exactly two incident faces, both planar, same `Face::dependency` solid, centroids distinct. **Non-manifold** edges (3+ faces) are skipped.

---

## Update: interior ribs preview (2026-05-10)

**Geometry:** `StructurePreview::BuildInteriorFaceRibs` — per planar **outer loop only** (`face.loops.size() == 1`), build orthonormal `(u,v)` in the face plane, clip **parallel chords** to the polygon in uv (convex clip via line–edge intersections; non-convex layouts may be imperfect). Spacing along each axis from `RibPreviewParams::spacingMm`; each chord becomes a **rectangle** of GL lines: chord on the face + inward offset `depthMm` along **−outward normal**. **Cap** 24 ribs per direction per face.

**UI:** Checkbox “Interior ribs (preview)” plus sliders spacing / depth / inset (margin fraction). Rib lines use `Color::GetAccent(2, …)` vs depth 1 for diamond/struts.

**Integration:** `SceneRenderer::SetStructureRibSegments`; `CommitStructurePreviewLinesToGpu` appends accent + rib colours into one Structure preview line mesh. `Display::RefreshStructurePreviewForRenderer` fills both segment lists. Overlay draws when **either** main preview lines or ribs are enabled.

**Update (suppress horizontal caps):** `BuildInteriorFaceRibs` skips planar faces whose outward normal satisfies `|n·(0,0,1)| > 0.9` (assume build-up **+world Z**) so lid/floor-style facets get no ribbons for now—FDM bridging/support + weak interlayer bending along Z—replace later with print-axis UI or alternate stiffening on those faces.

**Update (chord direction):** Lid skip alone leaves **horizontal** ribbons on **vertical** walls (one grid family runs parallel to XY). Each clipped chord is dropped when `|normalize(a1-a0)·ẑ| < 0.22` so surviving ribs lean “more vertical” in world space for **+Z** build-up.

**Chord end inset:** `RibPreviewParams::chordEndInsetMm` trims each clipped chord by that distance from **both** ends along the chord (UI “Chord end inset (mm)”, default 2). Ribs shorter than `2*inset + 0.6mm` are skipped.

**Inset face loop:** `BuildInsetFaceLoops` runs only on **horizontal-ish** planar faces (`|outward_normal·+(0,0,1)| ≥ 0.82`); vertical walls / steep normals are skipped. Loop points are shrink-wrapped uniformly in-plane about the centroid (same bounding-box-like scale on both tangent axes); inner edges extrude inward via **`AppendRibRectangle`** and **Inset extrude (mm)** (−outward normal, rib-style, `GetAccent(3)`).

**Correction (2026-05-10):** Replaced earlier **vertical-wall-only** inset (projection of **+Z** in-plane) with the policy above — user intent is **lid/floor** inset square-in-square style, not side-wall strips.

**Outcome (horizontal inset flip):** `cmake --build` clean; committed as `fix(structure): inset face loops on horizontal caps only`. Threshold `kTreatFaceHorizontalMinAbsNormalDotZ = 0.82` is tunable if sloped lids should be excluded or included.

**Mini retro:** Reused the rib `(u,v)` + uniform-scale pattern already familiar from sketch geometry; aligning “horizontal” with world **+Z** matches ribs’ cap skip heuristic but differs slightly (`0.82` vs `0.9`) — optional follow-up: one shared constant or build-axis param.

---

## Update: inset full-depth extrusion + single-cap dedupe (2026-05-10)

**UI:** Checkbox “Inset full depth via bbox (one horizontal cap)”. When enabled, manual “Inset extrude (mm)” is disabled (depth is computed). Tooltip documents bbox heuristic and overlap policy.

**Behaviour:** From each remaining horizontal cap, extrusion depth = ray `(face centroid, −outward_normal)` clipped through an **epsilon-padded axis-aligned bbox** of the owning solid (`RayAabbInterval`), minus small epsilon. Same **hemisphere preference** as below avoids drawing both lids meet-in-the-middle when both qualify.

**Overlap:** Before geometry, scan the solid once: if **any** horizontal-ish face (`|n·ẑ|` threshold) has `n·ẑ ≥ threshold` **(+Z‑outward / “upper” hemisphere)**, then drop all horizontal caps with **`n·ẑ ≤ −threshold`** (lower hemisphere). Otherwise keep only lower hemisphere caps (covers open cup / floor‑only lids).

**Code:** `BuildInsetFaceLoops(..., extrudeFullDepthThroughSolid)`; helpers `RayAabbInterval`; `StructurePreview.hpp` docs.

**Outcome / follow-up:** Bounding box approximate true interior penetration; eventual ray-vs-mesh opposite hit would tighten shape fidelity. Depth uses face centroid ray, not each inner-loop vertex.

---

## 2026-05-11 — Decision: retire diamond / inset / center-strut preview

**Why:** Empirical print comparison on a 30 mm cube showed the custom infill (3D diamond + inset cap, 2 mm features) consumed ~2× the filament/time of standard slicer infill (gyroid). Scaled to 1.2 mm features it matched filament use but lost the ability to support a top surface — struts and inset walls are too sparse near the top for the slicer to bridge between them. The fundamental issue is that *space-filling* slicer patterns (gyroid, cubic, lightning) provide both stiffness and top-surface bridging for free, while a *load-path* pattern like ours has to add separate top supports to be functional, which erases any volume gain.

Custom structure can only win on parts with explicit structural intent: known load direction, large parts (≥ 100 mm extent), exposed / aesthetic structure, or parts that would otherwise be solid. The 30 mm cube hits none of those, and "general parts" land closer to the cube than to the win cases.

**Pivot:** Replace this feature with a face-triangulation tool that carves printable vertical voids out of otherwise-solid parts — framed as a structural-intent tool rather than an infill optimizer. Tracked in `structure_face_triangulation_2026-05-11.md`.

**Code disposition:**

- **Keep:** Structure tool wiring in `Display` (toolbar, panel, import prerequisite, cancel/accept footer, translucent shell + behind-depth wire pass), `SceneRenderer` preview line plumbing, the planar-face / centroid / orthonormal-frame / AABB / `RayAabbInterval` helpers in `StructurePreview.cpp`'s anonymous namespace, the structure split path in `OpenGLRenderer::DrawTriangles`.
- **Remove:** `BuildAdjacentFaceMidpoints`, `BuildInteriorFaceRibs`, `BuildInsetFaceLoops`, `BuildCenterStruts`, the `PreviewPattern` enum, the rib-segment buffer, and the always-on rib/preview fill in `RefreshStructurePreviewForRenderer`.
- **Update copy:** Panel subtitle "Save on printed weight with specialized infill" replaced as part of the triangulation work; the old copy made a promise that on average over arbitrary parts is false.

**Mini retro:**

- Building three independent generators (diamond, ribs, inset) without an underlying mechanical model meant each pattern grew its own heuristics and they never composed into a coherent system. The symptom showed up first as visual clutter, then as empirical filament-loss vs. baseline.
- The right validation was always "compare against the actual slicer output," not "does the preview look structural." Catching this earlier would have shortened the loop — proposed `best_practices.md` addition (Stage 4): *"For features that compete with an existing tool/pipeline, benchmark against it before locking the API."*
- Reusable infrastructure (Structure tool scaffolding, preview line plumbing, translucent shell pass) survived the pivot cleanly because the tool was always abstracted from its generators — that part of the architecture held up.
