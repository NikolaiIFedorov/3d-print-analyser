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
