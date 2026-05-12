# Async work roadmap — Phase 0 (inventory + policies)

**Date:** 2026-05-12  
**Parent plan:** [Architecture_AsyncWorkRoadmap.md](../Architecture_AsyncWorkRoadmap.md)  
**Contract:** [Architecture_UIThreadAndWorkers.md](../Architecture_UIThreadAndWorkers.md)

## Stage 1 — Problem / approach

**Goal:** Baseline what already runs off-thread vs what can still block the main loop, and capture explicit policies before Phase 1–2 refactors.

**Approach:** Static pass over `Display::Frame`, `TaskRunner` usage, import/analysis/structure paths, and renderer incremental APIs; grep for blocking primitives on UI-adjacent code.

## Policies (draft — align with code as we extend)

| Topic | Policy |
|--------|--------|
| Main thread | Owns SDL events (via `Input::handleEvents`), `Display::Frame`, ImGui, camera sync, GL draws; **starts** jobs, **polls** `TryTake`, **applies** results. |
| Default worker pool | `Display::taskRunner` — serializes queued jobs; used for import worker lambda, analysis jobs. |
| Structure CGAL carve | Separate `StructureCarveTaskRunner()` (1 worker, process-leaked) so a stuck boolean does not block import/analysis on the default runner. |
| Cancel on geometry dirty | `MarkGeometryDirtyAll` / style dirty paths cancel **pending analysis** and **pending structure carve** (see `display.cpp`); keep new jobs consistent. |
| Stale results | Discard applies when **generation / request id / scene pointer** no longer matches (import generation, analysis `requestId`, structure job id — follow existing patterns). |
| Routing “long” work | **By operation type** (import path, analysis, carve, …), not by predicted duration. |
| STEP / PLY / etc. | File dialog lists extensions; async path only imports **stl / obj / 3mf** — other extensions fail fast in worker with “Unsupported import format.” |

## Inventory snapshot

### A — Already async or offloaded (pattern to copy)

| Area | Mechanism | Notes |
|------|-----------|--------|
| File import (happy path) | `deferredImportPath` → `ProcessDeferredImportIfAny` → `taskRunner.Submit` → `TryTake` → `mainThreadPipeline` steps | Parse/merge on worker; attach/frame/update on main in slices. |
| Analysis | `pendingAnalysisTask` + `taskRunner.Submit`; `TryTake` in `Frame` | `AnalyzeScene` runs on worker with reporter. |
| Structure staging carve | `pendingStructureStagingTask` + `StructureCarveTaskRunner().Submit` | Staging built on main; CGAL loop on dedicated runner. |

### B — Main-thread time-slicing or budgeted work (intentional, watch limits)

| Area | Mechanism | Risk if abused |
|------|-----------|-----------------|
| GPU scene rebuild after analysis tint | `renderer.RebuildAllIncremental(..., 2.5ms, ...)` in `Frame` | Large scenes: many frames to converge; OK if budget honored. |
| Structure preview (2D CGAL preview) | `kStructurePreviewMaxFacesPerFrame` (8) via `AdvanceStructurePreviewBuild` / `TickStructurePreviewBuildIfNeeded` | Single pathological face can still spike one frame (doc’d follow-up: async preview or per-face budget). |
| Import apply | `mainThreadPipeline.Process(1.5)` ms budget per `Frame` | Large `UpdateScene` / attach steps are split across named enqueue steps. |

### C — Synchronous or legacy paths (candidates for Phase 2 or deletion)

| Area | Location / trigger | Notes |
|------|-------------------|--------|
| Legacy sync import | `Display::CompleteFileImport` | Comment: fallback; runs `STLImport`/`OBJImport`/`ThreeMFImport` on **caller** thread — avoid new call sites; consider removal once confident async path only. |
| Structure eligibility + pick prep | `Display::Render` path building `structureEligibleFacesCache` from pick tris | Bounded by pick mesh size; watch very dense pick geometry. |
| `FrameScene` / `UpdateScene` | Main-thread; invoked from import pipeline and elsewhere | Can be heavy for huge scenes — already partially paired with incremental rebuild after import. |

### D — Blocking primitives audit (quick)

| Pattern | `src/display` | `src/input` | `src/main.cpp` |
|---------|---------------|--------------|-----------------|
| `future::wait` / blocking `get` on UI path | None expected beyond `TaskRunner` internal `TryTake` discipline | N/A | N/A |
| `TaskRunner` / `Submit` / `TryTake` | Central to import, analysis, structure | No | No |

**Note:** `TaskRunner` worker uses `queueCv.wait` — OK on worker, not on main.

## Grep commands (re-audit)

From repo root (extend paths when new modules add jobs):

```bash
rg 'CompleteFileImport|TaskRunner|TryTake|Submit\\(|mainThreadPipeline|RebuildAll\\(|RebuildAllIncremental' src/display src/input src/main.cpp
rg 'AnalyzeScene|STLImport::Import|OBJImport::Import|ThreeMFImport::Import|TryApplyStructureCarve|BuildFaceTriangulationPreview' src/display
```

## Phase 0 outcome

- Baseline table and policies recorded (this file).
- Roadmap phases linked from [Architecture_AsyncWorkRoadmap.md](../Architecture_AsyncWorkRoadmap.md).

## Next (Phase 1–2)

1. ~~Phase 1: unify pattern (docs + analysis worker/poll helpers)~~ — see [async_work_roadmap_phase1_2026-05-12.md](async_work_roadmap_phase1_2026-05-12.md).
2. Pick **one** item from section **C** (likely **retire or isolate** `CompleteFileImport` callers, or **instrument** `UpdateScene`/`FrameScene` post-import) and track in a new implementation log when work starts.

## Mini retro (Phase 0 only)

- **Worked:** Existing async import/analysis/structure split maps cleanly to the roadmap; little conceptual gap.
- **Follow-up:** Re-run inventory after STEP/PLY or new importers land.
