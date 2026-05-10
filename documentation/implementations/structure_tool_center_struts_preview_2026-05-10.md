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
