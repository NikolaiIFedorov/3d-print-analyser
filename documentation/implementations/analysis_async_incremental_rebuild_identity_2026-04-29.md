# Async analysis vs incremental mesh rebuild — identity + tint apply (2026-04-29)

## Problem (observed)

After routing analysis tint through incremental `RebuildAll`, the UI sometimes froze during analysis and **analysis panel results never updated**.

## Root causes (theory → fix)

1. **`RebuildAllIncremental` compared `AnalysisResults*` addresses** to detect “analysis changed mid-flight”. `Display` holds `AnalysisResults` in a **stack local** that changes every frame, so the pointer differed across frames even for the **same** logical rebuild → incremental rebuild restarted every frame (`BuildingSolids` reset), never reliably finishing → async completion could stall applying UI verdict/tints indefinitely.

2. **Tint application gated on clean geometry dirty flags**: async completion stored tint data while `geometryDirtyAll` stayed latched for incremental rebuild, so **`readyAnalysisResult` was never merged** until geometry settled — compounded by (1).

## Fix

- `SceneRenderer::RebuildAllIncremental` now tracks a **stable `analysisIdentity`** (async request id from `Display`) alongside scene + results pointers; restart only when **scene or identity** changes mid-flight — notwhen the `results` stack address differs.
- Renamed **`readyAnalysisResult` → `pendingAnalysisTint`**: consume on `styleDirty` **even while** a full incremental rebuild is in progress; pass identity into incremental rebuild.

## Verification

- Clean build (`cmake --build …`).
- Load a medium/large STL, enable Analysis: panel should populate after worker completes without perpetual geometry dirty latch; no indefinite “analysis pending” when incremental rebuild spans many frames.

## Retro (short)

Pointer identity on stack-backed `AnalysisResults` was a subtle footgun; prefer explicit generation ids for anything that spans frames.

## Follow-up fix (same day)

- `RunPickNode` still had a synchronous guardrail fallback that could call `Analysis::AnalyzeScene` + `renderer.RebuildAll` on the main thread whenever dirty flags were latched at pick time.
- During incremental rebuild hand-off this path could reintroduce visible freezes and bypass normal async-analysis/UI result application cadence.
- Updated `RunPickNode` to **defer pick refresh while geometry/style are dirty** (skip + return) instead of forcing fallback full rebuild.

## Follow-up fix 2 (analysis not returning after hitch reduction)

- While incremental geometry rebuild was in flight, `Display::Frame` intentionally avoided launching async analysis (`renderer.FullRebuildInProgress()` guard).
- If rebuild then settled with no new dirty event, analysis launch could be skipped indefinitely.
- Added `pendingAnalysisAfterGeometryRebuild`:
  - set whenever analysis is needed but blocked by in-flight rebuild,
  - consumed as soon as geometry/style dirty flags clear, launching async analysis immediately.

## Follow-up fix 3 (analysis UI vanished next frame)

- In `Display::Frame`, the `else` branch after `hasAnalysisThisFrame` cleared flaw counts and verdict whenever `geometryOrStyleWork` was true.
- After async analysis applied, incremental rebuild often keeps `geometryDirtyAll` latched for many frames with `hasAnalysisThisFrame` false on those frames → the clear ran every frame and wiped the panel right after it populated.
- Gate that clear: only when `geometryOrStyleWork && !analysisEnabled` (non-Analysis tool / analysis off path still resets UI during geometry work).

## Follow-up fix 4 (viewport flaw tint stopped updating after param change)

- `RebuildAllIncremental` was only given `AnalysisResults*` on the **first** frame (`hasAnalysisThisFrame`).
- Continuation frames passed `nullptr` and identity `0`, so `SceneRenderer` treated that as a new snapshot and restarted the incremental session **without** analysis → meshes rebuilt with default colors while the panel still showed flaws.
- `Display` now keeps `activeAnalysisTintForRebuild` (+ matching identity) until incremental geometry rebuild completes, and passes that pointer on **every** continuation frame.
