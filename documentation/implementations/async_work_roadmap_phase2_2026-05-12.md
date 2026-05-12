# Async work roadmap — Phase 2 (vertical slice 1)

**Date:** 2026-05-12  
**Parent plan:** [Architecture_AsyncWorkRoadmap.md](../Architecture_AsyncWorkRoadmap.md)

## Stage 1 — Problem / approach

**Problem (Phase 0 §C):** `Display::CompleteFileImport` was documented as a legacy synchronous import fallback, blocking the caller thread and duplicating logic already covered by `ProcessDeferredImportIfAny` + `MainThreadPipeline`.

**Approach:** Grep for call sites; if none, **delete** the method and declaration rather than maintaining two paths.

## Audit

- `rg 'CompleteFileImport\\(' src` — **no callers** (definition + declaration only).

## Implementation

- Removed `CompleteFileImport` from `display.hpp` and `display.cpp` (~85 lines).
- **Docs:** Banner on [Architecture_FileImport.md](../Architecture_FileImport.md) stating live async path and removal; [Architecture_AsyncWorkRoadmap.md](../Architecture_AsyncWorkRoadmap.md) Phase 2 notes slice 1; Phase 0 inventory row updated; [trackpad_two_finger_pan_reliability_2026-04-28.md](trackpad_two_finger_pan_reliability_2026-04-28.md) footnote for `FlushImportInputEventTail`.

## Outcome

- Single supported import orchestration: **`deferredImportPath`** → **`ProcessDeferredImportIfAny`** → **`taskRunner.Submit`** → **`TryTake`** → **`mainThreadPipeline`** enqueue chain (cancel/generation behaviour unchanged).

## Next Phase 2 slices

- Instrument or budget **`UpdateScene` / `FrameScene`** on very large scenes if profiling shows spikes post-import.
- Optional: async Structure preview job (Phase 3 / contract follow-ups).

## Mini retro

- **Worked:** Dead-code removal is low risk when grep proves zero call sites.
- **Did not:** `Architecture_FileImport.md` body still describes an older synchronous flow in places — banner flags drift; fuller refresh is a separate doc pass.
