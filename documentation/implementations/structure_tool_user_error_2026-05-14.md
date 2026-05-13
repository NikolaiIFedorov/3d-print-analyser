# Structure + shared tool user error (internal / CGAL failures)

## Idea

Reuse the Calibrate phase-1 pattern (stable **code**, **message**, **related parameter label**, click-to-copy `[code] message`) for **Structure carve** failures returned from the worker or scene clone, with sanitized user-facing text and full detail left to logs / `SessionLogger`.

## Plan

1. Extract copyable error drawing into `ToolUserErrorFeedback.{hpp,cpp}` (`ToolUserErrorPayload` + `DrawToolUserErrorCopyBlock`).
2. Rename Calibrate state to `ToolUserErrorPayload` / `calibToolError` (same shape).
3. Map raw worker `firstErr` strings to `STRUCT_*` codes in `MapStructureCarveRawToUserError` (main thread only).
4. Add `structureToolError` + `StructToolError` paragraph on the Structure panel; reserve root `children` capacity before extra paragraphs so cached pointers stay valid.
5. Wire `PollStructureStagingTaskIfReady`, worker-exception catch, clone failure, and successful partial paths; clear on cancel, successful full apply, commit/restore, and new carve launch.

## Outcome

- Shared UI helper and payload type live under `src/display/rendering/UIRenderer/ToolUserErrorFeedback.*`.
- Calibrate continues to use the helper with ImGui scope id `calibErr`.
- Structure shows the error block when the Structure tool is active, a model is present, and `structureToolError` is set; codes include `STRUCT_MESH_*`, `STRUCT_BOOLEAN_FAILED`, `STRUCT_CGAL_EXCEPTION`, `STRUCT_SCENE_CLONE`, `STRUCT_WORKER_EXCEPTION`, etc.

**Build:** `cmake --build build` succeeds (CGAL enabled).

## Mini retrospective

- **Worked:** Mapping on the main thread keeps ImGui and string shaping off workers; reserving `uiStructure->children` before two `AddParagraph` calls avoids the known pointer-invalidation footgun.
- **Follow-up:** Optional `STRUCT_EMPTY_STAGING` path; tighten map table as new `fail()` strings appear; consider a single `activeToolError` if cross-tool summary UI is ever needed.
