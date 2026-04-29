## 2026-04-29 - Analysis/Calibrate processing cards (title + accent progress strip)

### Idea
- Analysis and Calibrate panels did not visibly communicate that async/background work was active.
- Replace regular panel rows with a single processing card while work is in-flight.
- Use title-only status text (no subtitle) plus an accent progress strip to match existing UI language.

### Plan
1. Add a reusable paragraph-level bottom progress strip primitive in `UIRenderer`.
2. Add one processing paragraph to Analysis and one to Calibrate.
3. Drive visibility and title text from existing async state (`pendingAnalysisTask`, queued/applying states, import/apply work).
4. Use determinate progress when available; otherwise indeterminate animation.

### Implementation notes
- Added `Paragraph::accentProgressBar` and `Paragraph::accentProgress01` to express progress state.
- `UIRenderer::renderParagraph` now renders a bottom accent track for progress:
  - determinate when `accentProgress01` is in `[0,1]`,
  - indeterminate animated segment when `< 0`.
- Added `Display::RefreshToolProcessingCards(...)` to centralize card state updates.
- Analysis card text now switches between:
  - `Queueing analysis...`
  - `Analysing faces...`
  - `Applying analysis...`
- Calibrate card text now switches between:
  - `Importing model...`
  - `Refreshing calibration...`
- While a card is visible, corresponding standard rows/sections are hidden.

### Validation
- IDE lint check on edited files: no errors.
- Build passes: `cmake --build build -j4`.

### Outcome
- Completed.
- Analysis and Calibrate now visibly communicate background processing with a consistent title + accent progress card treatment.

### Mini retrospective
- What worked: implementing progress at `Paragraph` level kept the feature reusable without adding new panel types.
- What to tighten later: if calibration gets a dedicated worker with explicit counters, we can drive a more accurate determinate bar there too.

## Follow-up refinements (same day)

- Added explicit analysis scene-apply phase messaging: `Rendering analysis...` while mesh rebuild applies analysis tint/results.
- Clarified stage semantics: the accent strip reflects the **current** stage card (queue/analyze/render/apply), not aggregate end-to-end percent.
- Fixed card composition:
  - anchored progress strip to card bottom with dedicated bottom inset,
  - increased processing-card padding so title and strip do not visually intersect on compact panel heights.

## Crash fix (same day)

- Investigated intermittent startup `segmentation fault` introduced after adding processing cards.
- Root cause: `std::vector` reallocation invalidated stored `Paragraph*` pointers.
  - Analysis panel originally reserved for 4 children; adding a 5th processing paragraph could reallocate.
  - Calibrate panel reserve for processing paragraph happened after grabbing pointers into prerequisites/parameters.
- Fix:
  - reserve analysis children for 5 up-front,
  - reserve calibrate children for +1 **before** `AddPanel`/pointer capture,
  - remove late reserve calls that could invalidate pointers.
- Validation: rebuild succeeds (`cmake --build build -j4`), lint clean on touched files.

## Assertion follow-up (same day)

- New assertion observed: `children.size() < children.capacity()` in `RootPanel::AddParagraph` during Calibrate processing row insertion.
- Root cause: `UIRenderer::AddPanel(const RootPanel&)` copies the panel, so reserve hints made on the local `calibPanel` were not guaranteed on the stored `uiCalibrate`.
- Fix: reserve `uiCalibrate->children` immediately after `AddPanel` (and before pointer capture / `AddParagraph`).

## Import progress in Files bar + stale analysis preset (same day)

### Problem
- Import progress should appear on a Files tab (e.g. `Importing Organizer`) rather than inside the Analysis tool card.
- With Analysis active during import, the panel could still show a previous **“No issues detected”** preset while the new file was being analysed.

### Root causes
1. `analysisBusy` was gated with `!analysisPanelHasResults`, so any non-empty verdict (including stale pass text) suppressed the processing card while async analysis was running.
2. Stale verdict lines were not cleared when a new import started.

### Fixes
- **Files bar:** `pendingImportTabStem` and `RebuildFileTabs()` add a `file_pending_import` tab before `+` with `Importing <stem>` and a bottom accent fill bar. Tab is removed when import finishes or fails; stem cleared in sync `CompleteFileImport` too.
- **Analysis tool:** Import is no longer shown on the analysis processing card (Files bar + status strip only).
- **Stale preset:** Clear `uiVerdict->values` and flaw row state when async import starts; remove verdict-based gating from `analysisBusy`.
- **Calibrate:** `statusStripImportBusy` no longer drives the full calibrate processing overlay.
- **Redraw:** `renderDirty` each frame while import is busy or `pendingImportTask` is set so the tab bar animates.

### Validation
- `cmake --build build -j4` succeeds.

### Analysis bar “restarts” while flaws visible (same day)

- Cause: phase ordering treated `pendingAnalysisTint` in the final `else` as “Applying” at a **higher** fill than “Rendering”, so the bar jumped **backward** when moving from apply → render; brief idle could also zero the carry.
- Fix: explicit order queue → worker → **tint** → **render** with monotonically non-decreasing `analysisUiProgressCarry01`, slow creep near the end, and reset carry only after **3 consecutive idle frames** to absorb one-frame gaps.

### Full bar while still “Analysing” / no results (same day)

- Cause: unconditional creep once `carry >= 0.75` let the bar approach 100% during **pendingAnalysisTask** while verdict/rows stayed hidden until `analysisBusy` cleared.
- Fix: per-phase **ceilings** (e.g. worker max ~62%); **creep only** during `analysisRenderingInScene`.
