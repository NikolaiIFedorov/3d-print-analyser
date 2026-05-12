# Architecture: UI thread vs worker threads

**Audience:** anyone adding tools, importers, analysis passes, or CGAL-backed features.  
**Implementation detail:** `src/utils/TaskRunner.hpp`, `src/display/display.cpp` (`taskRunner`, `TryTake`, `Submit`).

## Goal

Keep **windowing, input, and OpenGL** responsive. Anything that can **block for more than a frame** (disk I/O, large mesh passes, **CGAL** booleans / offsets / skeletons) must not run on the UI thread in a way that **waits** on unbounded work.

## Main thread (UI / render loop)

**May:**

- Read input, drive ImGui, issue GL draws.
- **Start** async work: `taskRunner.Submit(...)`, bump generation / job ids, set “busy” UI.
- **Poll** completion: `pending*Task->TryTake()` (non-blocking); if ready, **apply** results (swap scenes, update tints, clear flags).
- Run **cheap** logic: pick filters, mode switches, small B-rep queries, **cached** previews that stay within a tight budget.

**Must not:**

- Call `std::future::get()` / `wait()` on long work (use `TryTake` + deferred teardown; see `TaskRunner::TaskHandle`).
- **Join** worker threads from the hot path except where shutdown policy explicitly accepts a stall (see below).
- Call **heavy CGAL** (booleans, interior offsets, full-face bake on many faces) synchronously from `Frame()`, input handlers, or synchronous import paths.

## Workers (`TaskRunner`)

**Should:**

- Receive **snapshots**: cloned scenes, paths, POD parameters — not raw pointers into mutable `Scene` the UI thread is editing.
- Honour `CancellationToken` between coarse steps (e.g. per solid) where feasible; note that **single CGAL calls** may still run to completion.
- Return structured results (`AsyncImportResult`, `AsyncAnalysisResult`, `AsyncStructureStagingResult`, …) for the main thread to apply once.

**Shared policy:**

- One `TaskRunner` instance on `Display` (see `display.hpp`) serializes jobs on its worker queue; avoid ad-hoc `std::thread` for the same class of work unless there is a strong reason.
- **Structure staging carve** (`BeginStructureStagingSession`) uses a **separate** `TaskRunner` in `display.cpp` (`StructureCarveTaskRunner`) so a stuck CGAL boolean cannot block import or analysis on `Display::taskRunner`. That runner is **intentionally process-leaked** (never destroyed) so `~TaskRunner` never joins a worker that might not return during teardown; process exit reclaims resources.

## CGAL

- **Carve / boolean mesh work:** run inside a `Submit` job (e.g. Structure staging carve loop). Inputs are staging clones built on the main thread before submit.
- **2D structure preview** (`StructureTriangulation::BuildFaceTriangulationPreview`): runs on the UI thread, but `Display` **chunks** work across frames (`kStructurePreviewMaxFacesPerFrame`, `TickStructurePreviewBuildIfNeeded` after `RunPickNode`) so eligibility changes do not run unbounded CGAL in one frame. Further hardening: async preview job or time budget per frame.

## Job identity and stale results

Use monotonic **generation** or **job id** fields; in `TryTake` handlers, **discard** results that do not match the current id (see `structureStagingIssuedJobId`, analysis `requestId`, import generation). Prevents applying work after tab switch, cancel, or superseding submit.

## Shutdown and `TaskRunner` destruction

- `TaskRunner::~TaskRunner` **joins** worker threads after setting `stopRequested`. If a worker is inside a long CGAL call, **process exit** can still block until that job finishes (queue drains).
- `TaskHandle` defers `future::get()` when discarding a **non-ready** handle so **assignment / reset** on the UI thread does not wait; detached threads may still complete after `Display` is torn down — acceptable for process exit; avoid if you need strict lifetime guarantees for tests.

## Audit (how to re-check)

From repo root, periodically:

```bash
rg '\.get\(\)|\.wait\(|\.join\(|TryTake|TaskRunner|Submit\(' src/display src/input src/main.cpp
```

Interpretation: smart-pointer `.get()` is fine; `future`-style blocking on the UI path is not. Extend search to new modules if they touch CGAL or import.

## Related docs

- `documentation/Architecture_AsyncWorkRoadmap.md` — phased plan (Phase 0 inventory, slices, chunking).
- `documentation/Architecture_FileImport.md` — import pipeline and progress.
- `documentation/Architecture_Analysis.md` — analysis stages vs worker.
- `documentation/implementations/ui_thread_worker_contract_2026-05-12.md` — audit snapshot and follow-ups.
- `documentation/implementations/async_work_roadmap_phase0_2026-05-12.md` — Phase 0 hotspot table and policies.
