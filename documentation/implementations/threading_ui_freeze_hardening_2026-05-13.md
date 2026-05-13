# Threading UI freeze hardening (2026-05-13)

## Goal

Reduce UI-thread stalls tied to async workers: bounded waits on futures, shared per-frame poll budget, non-blocking import progress snapshot, and explicit documentation of the analysis pipeline handoff.

## Changes

1. **`TaskRunner` / `TaskHandle` (`src/utils/TaskRunner.hpp`)**
   - `kUiAsyncFutureCompletionBudget` (16 ms): one ~60 Hz frame slice for UI-side future waits.
   - `TryTake(std::chrono::milliseconds maxWait = 0)`: optional bounded wait; default preserves prior poll-only behavior for ad-hoc callers.
   - `ReleaseFutureNonBlocking`: after a zero wait, one bounded `wait_for(kUiAsyncFutureCompletionBudget)` before spawning the detached waiter so fast-finishing tasks often complete without a helper thread.

2. **`Display::Frame` (`src/display/display.cpp`)**
   - At frame start, set `workerFuturePollDeadline = now + kUiAsyncFutureCompletionBudget`.
   - `ProcessDeferredImportIfAny`, `PollStructureStagingTaskIfReady`, and `PollPendingAnalysisTaskIfReady` use `TryTake(WorkerFuturePollRemainingMs())` so the three polls **share** one budget per frame.

3. **Import progress (`display.cpp` / `display.hpp`)**
   - `ApplyImportProgressSnapshot` / `ClearPendingImportProgressSnapshot` use `std::try_to_lock` on `importProgressMutex` so the UI thread never blocks on the worker’s brief `PublishImportProgress` lock; progress may skip one frame under contention.

4. **Analysis (`src/logic/Analysis/Analysis.hpp`)**
   - Document that `AnalyzeScene` copies analyzers under `pipelineMutex` then releases before scene work (existing behavior; deadlock avoidance).

## Outcome

Clean `cmake --build … --target CAD_OpenGL`.

## Mini retro

Bounded `TryTake` shares one 16 ms slice across three polls—tunable if a single pipeline needs more bias. Cooperative CGAL cancel remains follow-up.

## Follow-up (2026-05-13) — structure carve cancel + shutdown

- **`TryApplyStructureCarve`**: optional `shouldAbort` callback; checked after soup build, each face, before `BuildCarveFootprintOuterRingsWorld`, and each ring before prism/boolean work. **Does not** interrupt `corefine_and_compute_difference` mid-call.
- **Worker lambda**: passes token-backed `shouldAbort`; on `TryApply` failure, if cancel requested treats as `out.cancelled` (quit / `CancelPendingStructureCarveJob`).
- **Shutdown**: `AbandonStructureCarveTaskRunnerAtShutdown()` — lazy `TaskRunner*` for structure queue; after `CancelPendingStructureCarveJob`, calls `RequestStopClearQueueAndDetachWorkers` so the structure worker is never `join()`’d at process exit and queued follow-up jobs are dropped.

## Follow-up (2026-05-13) — “Carving…” stuck, no CGAL error in UI

**Cause:** `PollStructureStagingTaskIfReady` consumed the future (`reset`) then **returned early** (stale `jobId`, `cancelled`, wrong tab/tool, empty `staging`) **without** clearing the **Carving…** trailing caption — looks like a hang or “join,” but the worker may already be done.

**Fix:** Clear structure panel trailing + mark dirty on every discard path; `try/catch` around `TryTake` so worker exceptions replace Carving with a short failure caption.

## Follow-up (2026-05-13) — “Carving…” long after CGAL stderr

**Cause:** `std::future` is not ready until the worker lambda returns. The worker logged `LOG_WARN` *after* the carve loop (before `return out`), so **`cout` could block** while CGAL had already printed to stderr — UI still showed **Carving…**.

**Fix:** Remove that worker-thread `LOG_WARN`; main thread already logs on the all-failed path in `PollStructureStagingTaskIfReady`. For partial success, log `LOG_WARN` from the **main** thread when `firstErr` is set (same sanitized message), so session/terminal still get a line without blocking the future.

## Follow-up (2026-05-13) — Structure tool opens with no UI update (freeze before first paint)

**Cause:** `pendingToolSwitch` ran `BeginStructureStagingSession()` **before** `ImGui::NewFrame()` in `Render()`. That path did `Scene::Clone` and face grouping on the **main thread** first, so the window could not show the Structure panel or any busy caption until clone finished.

**Fix:** CGAL path splits scheduling vs work: `BeginStructureStagingSession` only validates, cancels any prior job, sets `pendingStructureStagingCarveLaunch`, and shows **Preparing carve…**; `FlushPendingStructureStagingCarveLaunchIfAny` runs at the **start** of the next `Frame()` (after `PollStructureStagingTaskIfReady`) and performs clone + submit + **Carving…**. Footer busy state treats the launch flag like an in-flight carve. `CancelPendingStructureCarveJob` clears the launch flag.

## Follow-up (2026-05-13) — Freeze + CGAL spam on simple model while “Carving”

**Cause:** The carve worker runs CGAL 3D (`StructureCarve`), but `TickStructurePreviewBuildIfNeeded` could still run **`StructureTriangulation::BuildFaceTriangulationPreview`** on the **UI thread** whenever staging was not active yet (`IsStructureStagingActive()` false while `pendingStructureStagingTask` held a future). That is **two threads in CGAL** (2D preview vs 3D corefinement), which is unsafe and matched “freeze right as CGAL logs.”

**Fix:** Skip Structure preview bake / refresh paths while `pendingStructureStagingTask` or `pendingStructureStagingCarveLaunch` is set (CAD_USE_CGAL); clear preview segments so the overlay does not fight the worker.

## Follow-up (2026-05-13) — Session log: `structure_staging_worker_result`

**Change:** On the **main thread** when `PollStructureStagingTaskIfReady` consumes a finished carve future, call `SessionLogger::LogStructureStagingWorkerResult` (wraps `PushEvent`) with `job_id`, `issued_job_id`, `job_id_matches`, `cancelled`, `carved_solids`, `carve_attempts`, `has_staging`, `has_first_err`, `first_err_snippet` (truncated), `target_scene`. Packaged-task exceptions call `LogStructureStagingWorkerException`. When the carve job is **queued** (`LaunchStructureStagingCarveJob` immediately after `Submit`), `LogStructureStagingJobSubmitted` records `job_id`, `target_scene`, `solid_carve_groups`, `eligible_face_count` (worker has not returned yet). The **worker thread** then logs **`structure_staging_worker_started`** (same `job_id`) as soon as the lambda runs, with eager flush so it hits disk before CGAL. If you see **submitted** but not **started**, the queue/thread never ran the job; if you see **started** but not **worker_result**, CGAL / carve code is blocking the worker. Compare `session_log.json` between models after quit or with eager flush.

## Follow-up (2026-05-13) — Identical `structure_staging_worker_result` on good vs bad model

**Interpretation:** That event only means the **worker** finished similarly; a freeze can still happen on **apply** (`UpdateScene` / GPU incremental rebuild / pick) or later. Identical `t_ms`/`dt_ms` across two exports usually means the same file or copy-paste, not two runs — fixed by **`session_run_id`** in JSON (unique per `SessionLogger::Start()`).

**Fix:** `LogStructureStagingApplyPhase` breadcrumbs: `discarded_*`, `apply_all_carves_failed_on_scene`, `applied_scene_swap`, `applied_after_update_scene`. If the log shows `applied_scene_swap` but not `applied_after_update_scene`, the hang is inside **`UpdateScene()`** (geometry path).

## Session log vs force quit

`session_log.json` is normally written once in `main::Shutdown` (`SessionLogger::Flush`). **Force quit / kill** skips `Shutdown`, so the file on disk can still be from the **previous clean exit** — identical `session_run_id` and events usually means you are not looking at the frozen run.

**Path:** `Flush` uses the path string as given to `std::ofstream` — **`session_log.json` is relative to the process current working directory** (often the `build/` folder when you run `./CAD_OpenGL` there, not the repo root). If you do not see a new file, check the cwd you launched from.

**Env (eager disk sync):** Set **`CAD_SESSION_LOG_EAGER_FLUSH=1`** or **`CAD_SESSION_LOG_FLUSH_AFTER_STRUCTURE=1`** (either name; non-empty value whose first character is not ASCII `0`) **in the environment of the running app** (export in the same shell you use to launch, or your IDE’s launch env). Then:

- **`MaybeFlushAfterImport`** runs after each **`LogFileImport`** — import alone creates/updates the log.
- **`MaybeFlushAfterStructurePoll`** runs when a Structure poll **returns** after consuming a result (RAII), **and** once **immediately before `UpdateScene()`** after `applied_scene_swap` so a hang **inside** `UpdateScene()` still leaves a log with `applied_scene_swap` but without `applied_after_update_scene`.
- After **`LogStructureStagingJobSubmitted`** (carve queued), another eager flush runs so you see **`structure_staging_job_submitted`** on disk even if the app never reaches **`structure_staging_worker_result`** (worker stuck or killed).

If the variable is only set in a terminal but the app is started from the Dock / another tool, it will not see it.

