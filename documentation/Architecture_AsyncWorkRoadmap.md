# Architecture: async work roadmap (main thread vs workers)

**Audience:** contributors extending tools, importers, CGAL-backed features, or anything that can stall the frame.  
**Companion (normative contract):** [Architecture_UIThreadAndWorkers.md](Architecture_UIThreadAndWorkers.md) — what the UI thread may and must not do, `TaskRunner`, shutdown, job identity.  
**Living inventory:** [implementations/async_work_roadmap_phase0_2026-05-12.md](implementations/async_work_roadmap_phase0_2026-05-12.md) — Phase 0 audit snapshot (re-run when adding heavy paths).  
**Phase 1 rollout:** [implementations/async_work_roadmap_phase1_2026-05-12.md](implementations/async_work_roadmap_phase1_2026-05-12.md).

## Intent

Move toward **no unbounded work on the main interaction thread** by:

- Keeping **windowing, input, ImGui, and OpenGL submission** on the main thread.
- Running **disk + parse + CGAL + large mesh passes** as **jobs** on workers, with **staging** and **main-thread apply** (and optional **time-sliced** steps where we own the loop).
- Classifying work **by operation kind** (import, analysis, structure carve, …), not by predicting wall-clock time per machine.

Existing building blocks: `TaskRunner`, `MainThreadPipeline`, async import apply chain, async analysis, dedicated Structure carve runner, incremental geometry rebuild and Structure preview baking (see Phase 0 doc for pointers).

## Phases (execution order)

### Phase 0 — Inventory and policies

- Enumerate **synchronous** or **main-thread-heavy** paths and **already-async** paths.
- Record **concurrency policies**: default `Display::taskRunner` vs `StructureCarveTaskRunner`, cancel-on-dirty behaviour, generation / request ids.
- **Re-run** Phase 0 after major features (grep patterns in companion doc).

### Phase 1 — Unify the pattern (**done 2026-05-12**)

- New heavy features use **`Submit` + `TryTake` in `Display::Frame()`** (or the same lifecycle) and **`MainThreadPipeline`** for multi-step main-thread apply when needed.
- Prefer **one bounded pool** per concern; separate runner only when a job class can **starve** others (already the case for Structure carve).

**Shipped in this pass:**

- `Display` documents the job stack next to `taskRunner` / `mainThreadPipeline` (pointer to this roadmap + UI-thread contract).
- Analysis: single **`ProduceAsyncAnalysisFromScene`** implementation for both `Submit` call sites; **`PollPendingAnalysisTaskIfReady`** mirrors the import/structure “poll on frame” pattern (`display.hpp` / `display.cpp`).

### Phase 2 — Vertical slices

- Pick **one** remaining hotspot from Phase 0; move it behind the standard job + apply pattern.
- Ship with **cancel / stale-result** guards (generation or scene pointer + id, matching import/analysis patterns).

### Phase 3 — Cooperative chunking (optional, targeted)

- For **owned** hot loops that still run on main: time-slice or **N-items-per-frame** (Structure preview already caps faces per frame).
- **Libraries** without incremental APIs: keep **whole-call async**, not internal chunking.

### Phase 4 — Hardening

- Overlap stress (import vs analysis vs structure), shutdown behaviour, optional coarse timing logs for regressions.

## Success criteria (pragmatic)

- Interactive path: **no routine multi-frame stalls** from known heavy tools on the main thread; worst cases **degrade** to progress + cancel where implemented.
- No **blocking `wait` / `get`** on long work from `Frame()` or input handlers (see contract doc).

## Related

- [Architecture_FileImport.md](Architecture_FileImport.md)
- [Architecture_Analysis.md](Architecture_Analysis.md)
- [implementations/async_work_roadmap_phase0_2026-05-12.md](implementations/async_work_roadmap_phase0_2026-05-12.md)
- [implementations/async_work_roadmap_phase1_2026-05-12.md](implementations/async_work_roadmap_phase1_2026-05-12.md)
