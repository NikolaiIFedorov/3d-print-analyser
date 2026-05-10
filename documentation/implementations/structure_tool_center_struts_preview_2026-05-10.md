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
