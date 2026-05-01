## 2026-05-01 — Analysis progress bar: numerator / denominator

### Idea
- Replace heuristic phase “bands” + creep with measurable **current / total** progress.
- Align with loops in `AnalyzeScene` (each counted unit ≈ one pass through an outer-loop body: solid face, loose face, solid-analyzer invocation, edge-analyzer invocation).
- Extend the denominator to cover the whole analysis pipeline visible in the UI: queue → worker (`AnalyzeScene`) → delivering results (`pendingAnalysisTint`) → incremental GPU rebuild (per solid + trailing stages).

### Plan
1. Add optional progress reporter + `CountAnalyzeSteps` on `Analysis::AnalyzeScene`.
2. Extend `Paragraph` + `UIRenderer`: when `accentProgressDenominator > 0`, draw determinate strip from numerator/denominator; keep `accentProgress01` / indeterminate as fallback.
3. `SceneRenderer`: count incremental rebuild steps (`solids built` + loose/repack/upload/pick).
4. `Display::RefreshToolProcessingCards`: recomputed denominator each busy frame from scene counts; numerator = worker steps (−GPU) + tint step + cached GPU incremental progress.

### Outcome
- Analysis card shows a single bar summed over pipeline phases; titles still reflect coarse phase (`Queueing`, worker sub-phase labels, `Applying`, `Rendering`).
- Worker reports phase id for subtitle granularity during `AnalyzeScene`.

### Caveats / follow-ups
- Face/solid/edge units are uniform steps, not weighted by analyzer cost — good for mirroring loops, imperfect for perceived “time remaining”.
- Import / Calibrate strips still indeterminate unless wired to STL byte/triangle counters later.

### Validation
- `cmake --build build -j4` passes.
