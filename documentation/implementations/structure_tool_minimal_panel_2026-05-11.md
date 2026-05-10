# Structure tool — minimal panel (2026-05-11)

## Idea

Structure preview tuning is mostly file-invariant; remove per-tool ImGui controls while keeping diamond (`BuildAdjacentFaceMidpoints`) and inset face loops with the same visualization (including translucent shell default).

## Plan

- `Display`: drop preview/rib/pattern toggles and rib float params; keep inset mm / depth / full-depth defaults and translucent shell default on.
- `RefreshStructurePreviewForRenderer`: when Structure tool panel is visible and scene has solids, always fill main preview + inset segments; clear rib segments.
- `BuildToolPanel`: Structure `parameters` empty; subtitle explains behavior.

## Outcome

Implemented in `src/display/display.cpp` and `src/display/display.hpp`. `cmake --build build --target CAD_OpenGL` succeeded (2026-05-11).

## Mini retro

Centralizing “always on” preview in `RefreshStructurePreviewForRenderer` is clearer than leaving dead toggles in `Display`. If tuning returns, prefer app Settings or a dev-only overlay over resurrecting a large tool panel.

---

**2026-05-11 follow-up:** Default `structureInsetFaceFullDepthThroughSolid` to `true` so inset preview extrudes through the solid via the existing bbox heuristic. Structure panel subtitle set to user-facing copy: “Save on printed weight with specialized infill”.

**2026-05-11 follow-up:** Restored a single **Import a file** prerequisite on the Structure `ToolPanelDef` (same `calibStepImport` + `DoFileImport` pattern as Calibrate). The minimal panel earlier had an empty `prerequisites` vector, so the section rendered with no rows. `structPara_Import` mirrors Calibrate’s hide-on-import behavior.

**2026-05-11 follow-up:** `ToolPanelDef` gained optional `hasSceneEditFooter` + `sceneEditFooter` (`ParameterDef`); `BuildToolPanel` appends a root-level ImGui row. Structure uses **Cancel** / **Accept** calling `Display::FinalizeStructureSceneToolSession` (currently both leave Structure for Analysis; `accepted` reserved for future commit vs discard).

**2026-05-11 follow-up:** Scene-edit footer paragraph is **hidden until import is done** (`structPara_SceneEditFooter` + `SyncStructurePanelDerivedVisibility`). Cancel/Accept drawn as two **settings-style text pill** segments (hover accent, rounding aligned with `UIRenderer` `Select`). `computeParagraphBox` returns zero content when `Paragraph::visible` is false so hidden footer does not reserve vertical space.
