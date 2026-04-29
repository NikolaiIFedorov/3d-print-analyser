# STL import timing breakdown (small-model latency triage)

## Context

- User reported noticeable import delay even for a small STL (~40 points).
- Current STL import flow can spend time in multiple stages:
  - parse + topology build,
  - optional coplanar-face merge,
  - post-import display flow (framing + dirty scheduling),
  - next-frame analysis/rebuild work.

## Implementation plan

1. Add explicit timing to `STLImport` for:
   - parse/build stage,
   - coplanar merge stage,
   - total STL import time.
2. Return a small stats struct from `STLImport::Import` via optional out-parameter
   so callers can log stage breakdown without changing behavior.
3. Add timing in `Display::CompleteFileImport` for:
   - importer call,
   - `FrameScene`,
   - `UpdateScene`,
   - total import pipeline inside this function.
4. Emit one concise summary log line after STL import.

## Notes during implementation

- Keep existing behavior unchanged (same import outputs and merge semantics).
- Keep instrumentation low-risk and cheap (`std::chrono::steady_clock` + single summary log).

## Outcome

- Added STL stage timing + summary stats (`STLImportStats`).
- Added display-side import pipeline timing logs for STL imports.
- This gives immediate visibility into whether latency comes from STL parse/merge or post-import work.
- Made verbose analysis timing logs opt-in (`kLogAnalysisTimingDetails = false` in `Analysis.cpp`) to avoid
  synchronous terminal I/O stalls during normal import/interaction.
- Moved import parse/build off the UI thread in `Display::ProcessDeferredImportIfAny`:
  - file parsing now runs in an async worker (`std::async`),
  - main thread polls completion each frame,
  - scene swap + camera/UI/session updates occur only when worker result is ready.
- Expected impact: native file dialog return and app interaction remain responsive while import work executes.
- Added two-phase post-import rebuild in `Display::Frame`:
  - first geometry rebuild after import skips expensive analysis once (`skipAnalysisForNextGeometryRebuild`),
  - a deferred follow-up style/analysis pass is scheduled for the next frame (`pendingAnalysisAfterImportRebuild`).
- Expected impact: reduce single-frame hitch at the exact moment import completes.
- Identified major startup/idle terminal spam from per-frame renderer logs:
  - `OpenGLRenderer::DrawTriangles` and `DrawLines` logged every frame, including zero-index draws.
- Disabled these per-frame logs by default (`kLogDrawCalls = false`) to avoid terminal I/O stalls and blank-line noise.
- Identified additional import-time spam from `Scene` construction/merge logs.
- Disabled `CreatePoint/CreateEdge/CreateFace/CreateSolid` and merge debug logs by default via:
  - `kLogSceneConstruction = false`
  - `kLogMergeDebug = false`
  in `src/scene/scene.cpp`.
- Added a log-level gate in `Log::Write` so verbose runtime levels are off by default:
  - suppress `BACKGROUND`, `DEBUG`, `DESC`, `INFO`,
  - keep `SESSION`, `WARN`, and `ERROR`.
- Goal: remove terminal I/O pressure as a root cause of startup/import stalls and blank-line output artifacts.

## 2026-04-29 cleanup update (logging profiles)

- Replaced ad-hoc verbose gating with a proper verbosity profile API:
  - `LogVerbosity::{QUIET, NORMAL, VERBOSE}` in `log.hpp`,
  - `Log::SetVerbosity(...)` / `Log::GetVerbosity()`.
- Centralized filtering in `Log::Write` via `ShouldEmit(level)` for readability and future maintenance.
- Added env override in `Log::Start`:
  - `CAD_LOG_VERBOSITY=quiet|normal|verbose` (default `normal`).
- Set app startup to explicit normal profile in `main.cpp`.

## 2026-04-29 architecture draft update (TaskRunner)

- Added reusable `TaskRunner` utility: `src/utils/TaskRunner.hpp`.
  - single/multi worker support (`workerCount`, default 1),
  - queued jobs with worker-thread loop,
  - typed `TaskHandle<T>` with non-blocking `TryTake()`,
  - cancellation token (`RequestCancel()` + `IsCancellationRequested()`).
- Refactored import async flow in `Display` to use `TaskRunner` instead of direct `std::async`.
  - keeps existing import behavior and stats,
  - adds explicit cancellation semantics for superseded import tasks,
  - centralizes task lifecycle handling for follow-up async workloads (analysis precompute, etc.).

## 2026-04-29 follow-up update (analysis TaskRunner pass)

- Added async analysis task wiring in `Display` using `TaskRunner`:
  - new `AsyncAnalysisResult` payload and `pendingAnalysisTask` / `readyAnalysisResult` state.
- Post-import deferred analysis (`pendingAnalysisAfterImportRebuild`) now queues an analysis job on worker thread instead of running synchronously on the next frame.
- When analysis result becomes ready:
  - main thread stores result,
  - schedules style/analysis/pick/UI invalidation,
  - applies result through existing renderer/UI pass (recolor + flaw summary) without blocking import completion frame.
- Synchronous analysis remains as fallback for normal geometry/style edits.

## 2026-04-29 architecture update (main-thread budgeted pipeline)

- Added reusable `MainThreadPipeline` utility: `src/utils/MainThreadPipeline.hpp`.
  - queue of staged main-thread tasks,
  - per-frame budgeted processing (`Process(budgetMs)`),
  - clear/reset support for superseded work.
- Integrated pipeline into `Display::Frame` and import handoff:
  - import apply now runs in staged steps across frames:
    1) attach scene,
    2) frame scene,
    3) mark/update scene,
    4) finalize UI/session/log/status.
  - clears pending staged tasks when a new import starts.
- Render invalidation work is deferred while a staged apply step runs in the same frame to reduce hitch risk.

## 2026-04-29 profiling update (pre-render freeze triage)

- Added low-noise slow-stage timing instrumentation in `SceneRenderer::RebuildAll`:
  - `build_chunks`, `rebuild_loose`, `repack_offsets`, `upload_all_packed`,
    `rebuild_pick_tris`, `rebuild_pick_segs`, `rebuild_all_total`.
- Added GPU upload timing in `OpenGLRenderer`:
  - `UploadTriangleMesh` (`triangles_full`),
  - `UploadLineMesh` (`lines_full`).
- Logging policy:
  - emit only when stage duration >= 4 ms,
  - use `SESSION` level so timings remain visible under normal verbosity.

## 2026-04-29 render optimization update (incremental full rebuild)

- Implemented `SceneRenderer::RebuildAllIncremental(...)` to time-slice solid chunk generation across frames.
- `Display` now uses incremental rebuild for `geometryDirtyAll` with a per-frame CPU budget (~2.5ms),
  keeping `geometryDirtyAll` latched until completion.
- `RunPickNode` skips guardrail escalation while a full rebuild is in progress (`FullRebuildInProgress()`),
  preventing accidental synchronous `RebuildAll` loops during partial uploads.
- `RebuildAll` / `RebuildSolids` / `RecolorOnly` abort any in-flight incremental session for correctness.

## Outcome (2026-04-29)

- Import responsiveness improved materially once synchronous terminal logging and per-frame renderer logs were removed/gated.
- Added structured async primitives (`TaskRunner`, `MainThreadPipeline`) and wired import + post-import analysis scheduling through them.
- Added render-stage profiling hooks to isolate pre-render stalls.
- Implemented incremental full-scene mesh rebuild for `geometryDirtyAll` to reduce single-frame spikes on large solids counts.

## Mini retrospective

- What worked: separating “off-thread compute” vs “main-thread apply” concerns made the system easier to reason about and extend.
- What was harder than expected: pick-node guardrails interacted with partial rebuild state; needed explicit `FullRebuildInProgress()` gating.
- Follow-up: time-slice `RebuildLoose` / very large `UploadAllPacked` if profiling shows those dominate; consider moving viewport reads out of mesh generation for future worker offload.

## 2026-04-29 follow-up (analysis hitch when results arrive)

- Observation from runtime traces:
  - render rebuild stages are ~20–35 ms on tested models,
  - major visible hitch aligns with analysis completion/apply cycle.
- Change made:
  - removed synchronous analysis execution from normal geometry/style render path,
  - now launch analysis via `TaskRunner` whenever analysis is needed and no result is ready,
  - apply only completed async results on main thread via the existing recolor path.
- Added request-id invalidation:
  - each geometry/style invalidation bumps `analysisRequestId`,
  - stale/cancelled analysis results are ignored.

## Mini retrospective

- What worked: adding optional stats avoided invasive API changes and kept existing call sites simple.
- What to improve next: once logs identify dominant stage, follow-up should target exactly one hotspot
  (likely merge or deferred analysis) to keep optimization risk low.
