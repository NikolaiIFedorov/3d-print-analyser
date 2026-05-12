# Async work roadmap — Phase 1 (unify pattern)

**Date:** 2026-05-12  
**Parent plan:** [Architecture_AsyncWorkRoadmap.md](../Architecture_AsyncWorkRoadmap.md)

## Stage 1 — Problem / approach

**Problem:** Analysis async work duplicated two large `Submit` lambdas in `Display::Frame()`, and the job stack (`taskRunner`, `mainThreadPipeline`, runners) was not pointed at the roadmap from code.

**Approach:** Document the stack in `display.hpp`; centralize analysis worker logic; name the analysis poll step like import/structure.

## Implementation

- `display.hpp`: Comment block before `taskRunner` / `mainThreadPipeline` linking `Architecture_AsyncWorkRoadmap.md` and `Architecture_UIThreadAndWorkers.md`.
- `display.hpp` / `display.cpp`:
  - `ProduceAsyncAnalysisFromScene` — shared body for `AnalyzeScene` on the worker (cancellation, reporter, results).
  - `PollPendingAnalysisTaskIfReady` — `TryTake` + stale guard + tint scheduling (extracted from `Frame()`).
  - Both `Submit` sites in `Frame()` call the producer via a short lambda.

## Files

- `src/display/display.hpp`, `src/display/display.cpp`
- `documentation/Architecture_AsyncWorkRoadmap.md` — Phase 1 marked done; Related links.
- `documentation/implementations/ui_thread_worker_contract_2026-05-12.md` — audit row tweak.

## Outcome

- One place to edit analysis worker behaviour; `Frame()` analysis poll reads like import/structure polling.
- New heavy features still: `Submit` → hold `optional<TaskHandle>` → poll in `Frame()` → apply; multi-step apply via `MainThreadPipeline` when needed.

## Mini retro

- **Worked:** Small refactor, no behaviour change intended; duplicate lambda removal lowers drift risk.
- **Did not:** Import worker lambda still monolithic (Phase 2+ if we want symmetry).
- **Follow-up:** Phase 2 vertical slice (e.g. `CompleteFileImport` callers) from Phase 0 section C.
