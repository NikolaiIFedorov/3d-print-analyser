# Structure carve: background worker (CAD_USE_CGAL)

## Problem / idea

- CGAL carve during `BeginStructureStagingSession` ran on the main thread and froze the UI until work finished.
- Move the **carve loop** (per-solid `TryApplyStructureCarve`) to `TaskRunner` after a **main-thread clone** + eligibility map so pointers into the staging scene stay valid for the worker payload.

## Plan

1. Add `AsyncStructureStagingResult`, `pendingStructureStagingTask`, `structureStagingIssuedJobId`; poll in `Frame()` like analysis/import.
2. `BeginStructureStagingSession`: clone + build `bySolid` on main thread; submit job; set header trailing `"Carving…"`; invalidate stale jobs when superseding or cancelling.
3. `RestoreStructureOriginalScene` / tool switch / `MarkGeometryDirtyAll`: cancel pending task and bump job id so late results never apply.
4. `TryCommitStructureFacePick`: while a carve job is pending, call `BeginStructureStagingSession()` again to cancel + restart with updated exclusions (same as sync `Rebuild` intent for pre-staging toggles).
5. Hide Structure Cancel/Accept footer while a carve job is in flight; clear trailing caption when cancelling.

## Outcome

- **Async carve:** With `CAD_USE_CGAL`, `BeginStructureStagingSession` clones and builds `bySolid` on the main thread, then runs the per-solid `TryApplyStructureCarve` loop on `TaskRunner`. `Frame` calls `PollStructureStagingTaskIfReady` to swap `ownedScenes[activeSceneIndex]`, set `structureOriginalScene`, apply header error text, and `UpdateScene` when the job id matches `structureStagingIssuedJobId` and the user is still on Structure for the same tab.
- **Invalidation:** `CancelPendingStructureCarveJob` (cancel + reset handle + bump job id + clear “Carving…” trailing) is used from `RestoreStructureOriginalScene`, `CommitStructureStagingScene`, tab switch, `MarkGeometryDirtyAll`, `MarkGeometryDirtySolid`, and leaving Structure via `pendingToolSwitch` so late results never apply to the wrong scene.
- **UI:** Header trailing shows `Carving…` while the worker runs; Cancel/Accept footer is hidden until the swap (`structureCarveBusy`). Exclusion toggles while a job is pending call `BeginStructureStagingSession` again to cancel and restart with the new exclusion set.
- **Mini retro:** Reused existing `TaskRunner` single-queue pattern (same as import/analysis) instead of a new thread type; cooperative cancel only between solids (CGAL may still run long inside one solid).

## Follow-up (2026-05-12)

- **Accept freeze:** After Accept, `Commit` leaves the same carved mesh in `ownedScenes` (only drops `structureOriginalScene`). The pending tool-switch handler still called `MarkGeometryDirtyAll()` whenever `analysisEnabled` already matched Analysis, forcing a redundant full incremental GPU rebuild. **Fix:** `structureFinalizeCommitSkipGpuFullRebuild` — on Accept, next switch uses `MarkPickDirty` + `MarkStyleDirty` instead so analysis can re-queue without replaying mesh rebuild.

- **Analysis / import “dead” after Accept:** `Commit` destroys `structureOriginalScene`; `analysisUiScene` could still reference that freed scene, so `analysisUiScene == scene` failed (Result/Verdict hidden; stale `lastCommittedAnalysisForRecolor` face pointers). **Fix:** On commit, cancel pending analysis, clear tint/verdict/flaw state, bump `analysisRequestId`, set `analysisUiScene = scene`. Same `analysisUiScene = scene` after carve swap in `PollStructureStagingTaskIfReady` and after restore in `RestoreStructureOriginalScene`.

- **CGAL `intersection_nodes` assertion (corefinement):** Mitigation in `StructureCarve.cpp`: `merge_duplicate_points_in_polygon_soup` after orient on solid + prism soups; `stitch_borders` on meshes after `duplicate_non_manifold_vertices`; clearer carve `errOut` when the exception mentions corefinement / assertion. Hard failures can remain on ill-conditioned extrusions.

## Follow-up (2026-05-12, second pass)

- **Freeze switching back to Structure / on quit + terminal “blank line” spam:** `TaskHandle::~TaskHandle` already deferred `std::future::get()` when the task was not ready, but **`TaskHandle` move-assignment stayed defaulted**. Replacing a handle (e.g. `optional::reset` / assign) ran `std::future::operator=`, whose **destruction of the previous shared state can block** the same way as `~future`. **Fix:** `ReleaseFutureNonBlocking()` shared by destructor and **`operator=(TaskHandle&&)`**; includes `<chrono>` / `<memory>` as needed.
- **Self-intersecting blue carve preview (L-shaped inner corner):** Fillet arc sampling on the **carved** 2D ring can cross itself at reflex geometry. **Fix:** In `StructureTriangulation.cpp` / `CollectCarvedFilletedRings3D`, after `FilletPolygonCorners`, run a cheap proper-segment intersection test on the closed ring; if it self-intersects, **fall back to the unfilleted** `outer2D` / hole ring before lifting to 3D so the footprint stays a simple polygon for preview and prism construction.
