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
