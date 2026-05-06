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

### Verdict/counts hidden while mesh already shows flaw tints (same day)

- Cause: `uiResult` / `uiVerdict` used `!analysisBusy`, and `analysisBusy` included `analysisRenderingInScene` (incremental GPU rebuild). Counts and verdict were filled when tint was consumed, but panels stayed hidden until `FullRebuildInProgress()` cleared. First `RefreshToolProcessingCards` in the invalidation block also ran **before** tint consumption.
- Fix: introduce `analysisPipelineWaiting` (task, queue, unconsumed tint only) and show result/verdict when the pipeline is not waiting; keep the full `analysisBusy` for the processing card and phase bar. Call `RefreshToolProcessingCards` again at the end of the invalidation block after analysis UI updates.
- Validation: `cmake --build build -j8` on `display.cpp`.

### Follow-up: panels still hidden (queueingFirstAnalysis)

- `queueingFirstAnalysis` is `pendingAnalysisAfterGeometryRebuild && !lastCommittedAnalysisForRecolor`; `lastCommitted` is only set when incremental rebuild completes. That made “pipeline waiting” effectively true for the whole post-worker GPU window when combined with the earlier gate.
- Panel visibility now uses **only** `pendingAnalysisTask` and unconsumed `pendingAnalysisTint`; `queueingFirstAnalysis` remains in `analysisBusy` for the processing card / staged bar only.

### Results visible one frame then hidden (duplicate worker)

- Cause: `shouldLaunchAsyncAnalysis` did not check `activeAnalysisTintForRebuild`. The frame after consuming `pendingAnalysisTint`, the worker path was eligible again while the prior run’s tint was still being applied incrementally; a second `AnalyzeScene` was submitted, `pendingAnalysisTask` became true, and panel visibility hid again.
- Fix: require `!activeAnalysisTintForRebuild.has_value()` for `shouldLaunchAsyncAnalysis` and for the top-of-frame deferred launch at `pendingAnalysisAfterGeometryRebuild`.

## 2026-05-01 — Build repair (`PickRef`, progress strip)

### Problem
- `display.cpp` referenced `CalibrateDistance::PickRef` and `Paragraph::{accentProgressBar,accentProgress01}` while headers/renderer had drifted: `CalibDistanceType` only exposes `Face *` helpers, and `Paragraph` in `Panel.hpp` no longer declared the progress fields.

### Approach
- **Calibration:** resolve picks with existing `ResolveCalibFaceForWorkflow` and call `CombinePickedFaces` / `ClassifyFace` with `const Face *` (remove `PickRef` helper).
- **UI:** restore `accentProgressBar` / `accentProgress01` on `Paragraph` and implement bottom track + determinate/indeterminate fill in `UIRenderer::renderParagraph` / `computeParagraphBox` (per original processing-card design).

### Outcome
- Clean compile and link after refresh; main files: `display.cpp`, `Panel.hpp`, `UIRenderer.cpp`.

## 2026-05-01 — Import prerequisite progress + strip alignment

- **Import:** Drive `uiImportPara->accentProgressBar` / `accentProgress01` from the same `statusStripImportBusy` / `pendingImportTask` / `statusStripImportProgress01` state as the Files tab strip (`RefreshToolProcessingCards`).
- **Layout:** Draw the bottom accent track in the padded content inset (`px0/px1` ± padding) and sit flush to the inner bottom (`py1 - pad`) so horizontal alignment matches body text and the extra gap under the bar is removed; tighten layout gap above strip (1px).

## 2026-05-04 — Calibrate pick-flow reset

### Problem
- After plotting one calibration measurement point, the derived/result parameter row could stay allocated even after leaving and re-entering Calibrate.
- The pick clear path recalculated calibration UI before resetting the step state, so `RefreshCalibDerivedRowVisible()` still saw point 1 as done.

### Plan
- Make clearing calibration picks reset the transient pick workflow in one place: picked face/edge pointers, point step states, selected prerequisite row, workflow/compensation, and derived row visibility.
- Reuse that reset when switching files, switching tools, hiding/showing Calibrate, and activating a newly imported scene.

### Outcome
- `ClearCalibrateFacePicks()` now resets the point step states and selected prerequisite before refreshing workflow/derived-row UI.
- The derived calibration row now waits for two valid picks, so one selection no longer reserves blank result space.
- Tool re-entry, file-tab switches, and imported-scene activation all use the same reset path.
- Validation: `cmake --build build -j8` passes.

### Mini retrospective
- Centralizing the reset removed several copy-pasted partial resets and fixed the ordering bug at the source.
- The derived row visibility had drifted toward "show after first point" while its content still needed two picks; keying visibility off the actual picked span is clearer.

### Follow-up: result spacing
- Calibrate now opts its Parameters section back into child splitters, so the computed result is visually separated from `Print measurement`.
- Tightened Calibrate parameter-row margins to reduce the extra bottom space left by the result/measurement block.

### 2026-05-04 — Calibrate point-pick independence

### Problem
- Clicking the second measurement-point prerequisite before the first point was plotted could deselect Point 1.
- The pick filter correctly rejected Point 2 until Point 1 was complete, but the UI then had no active eligible pick row, so edge/face hover and selection appeared disabled.

### Fix
- Treat Point 1 and Point 2 as independent measurement slots, not ordered prerequisites.
- Either slot can be selected and picked first; after a pick, selection advances only to the other empty slot.
- The derived/result row still waits until both slots have picks.

### Note
- The current compensation output still lives as `CalibDerived`, the second row in the Calibrate `Parameters` section. It is visually separated with child splitters, but it is not a separate `Result` section yet.

### Follow-up: flatten Calibrate measurement/result rows
- Calibrate is a sequential workflow rather than a settings panel, so `Print measurement` and the computed compensation result now render as direct panel rows after prerequisites.
- Added an explicit `ToolPanelDef::flattenParameters` option so future tools can choose flat rows without changing existing sectioned panels.
- The previous `Parameters` section path remains supported for tools that benefit from grouped settings.

## 2026-05-07 — Analysis result row: value hover vs “No …” label

### Problem
- For zero findings, the left label is drawn as `No` + plural (e.g. “No thin sections”), but `leftW` for the nav/param split was sized from `0` + singular (`0 thin section`), so the DragFloat / hover tint started too far left and overlapped the title.

### Approach
- Build `countBuf` the same way for layout and drawing: numeric + label when `count > 0`, else `No%s%s` with `countLabel` and `plural`.

### Outcome
- Param zone starts after the full “No …” text; hover no longer intersects the title.

### Mini retrospective
- Root cause was layout measuring a different string than paint; aligning buffer + width removed the overlap without new layout constants.

## 2026-05-07 — Calibrate committed edge pick: show edge highlight, not face fill

### Problem
- After picking an edge (elephant’s foot / cap edge workflow), the mesh still drew `appendFaceTris` for the stored owning face, so the selection read visually as a face pick even though `calibEdgePoint1/2` was set (edge lines were easy to miss under the face tint).

### Approach
- Match hover behaviour: if a slot has a committed edge (`calibEdgePoint* != nullptr`), skip face triangle highlights for that slot; keep `appendEdgeLines` for the edge.

### Outcome
- Edge-only visual for edge-snapped committed picks; face-only picks unchanged.

### Mini retrospective
- Reused the same rule already documented for hover (`calibrateEdgeHover`); committed state had been left drawing both layers.
