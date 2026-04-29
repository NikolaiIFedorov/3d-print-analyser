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
