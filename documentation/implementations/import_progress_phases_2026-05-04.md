# Import Progress Phases

Date: 2026-05-04

## Idea

The import progress strip currently uses `statusStripImportProgress01 = -1.0f`, which renders an
indeterminate animation. That is honest but confusing because the looping bar looks like progress
resetting. Replace the vague indicator with named import phases and real phase progress where the
importer has a reliable denominator.

## Plan

- Add a small import progress callback shared by STL, OBJ, and 3MF importers.
- Keep unknown phases indeterminate, but attach them to text such as "Merging coplanar faces...".
- Publish worker-thread progress into `Display` through a locked snapshot before the UI reads it.
- Update the status strip, pending file tab, and "Import a file" prerequisite label/bar from the same
  phase/progress state.

## Notes

- Binary STL has good denominators for bounds scan and triangle read passes.
- ASCII STL and OBJ can report approximate file-position progress.
- 3MF has coarse archive/XML/build phases; detailed mesh progress can be added later if needed.

## Outcome

- Added `ImportProgress` / `ImportProgressCallback` and optional callback parameters to STL, OBJ, and
  3MF importers.
- `Display` now publishes worker-thread import progress through a generation-checked locked snapshot,
  so stale progress from a cancelled import cannot overwrite a newer import.
- The status strip, pending file tab, Analysis "Import a file" prerequisite, and calibration
  processing card now show the current phase plus a percentage when available.
- Unknown phases still use the existing indeterminate bar, but the label now explains the phase.

## Mini Retro

The callback stayed small and matched the existing importer APIs without a registry refactor. Binary
STL gives useful real progress immediately; 3MF remains coarse because its mesh traversal does not yet
have cheap global counts. A future improvement would split `ImportProgress` into phase and total
progress if we want the bar to remain globally monotonic across every phase.

## Follow-Up: Determinate Merge Bar

Feedback: the "Merging coplanar STL faces..." label was useful, but the bar still used the unknown
looping animation. Updated the importer to treat progress as overall import-pipeline progress:

- Binary STL bounds scan maps to 2-15%, triangle read maps to 15-70%, coplanar merge maps to 70-85%,
  and main-thread attach/frame/update/finalize maps to 85-100%.
- `Scene::MergeCoplanarFaces` now accepts a generic progress callback and reports a monotonic estimate
  based on how many original faces have been removed, with a forced 100% when merge completes.
- OBJ and 3MF were adjusted to report overall pipeline slices too, so the bar does not jump backward
  between phases.

## Follow-Up: Prerequisite Row Copy

Feedback: replacing the whole prerequisite title with the phase made the row lose its stable meaning.
Updated the Analysis prerequisite row to show:

- title: `Import a file: 73%`
- subtitle: current import phase, e.g. `Merging coplanar STL faces...`

The status strip and Files tab still use the full phase-plus-percent label because those are status
surfaces rather than prerequisite steps.

## Follow-Up: Remove Status Strip Panel

Feedback: after the import progress bar became useful in the Files tab and tool prerequisite rows, the
top notification/status strip was unused. The intended future role is a timeline pop-up once the CAD
stage exists; until then it adds chrome without carrying unique user-facing state.

- Removed the `StatusStrip` root panel and its special Settings/Toolbar layout offset.
- Kept import progress as shared `Display` state for the Files pending tab, Analysis prerequisite row,
  and Calibrate processing row.
- Removed the idle debug status text path rather than moving those counters into normal UI.

Verification: `cmake --build build` passes.

Mini retro: removing the panel was cleaner after renaming the remaining progress fields away from
`statusStrip...`; otherwise the old panel concept would have lingered in the import state names.

## Follow-Up: Reimport Analysis Prerequisite

Bug: importing a second file after analysis had completed cleared the Analysis result/verdict state
while the previous scene was still active, but the "Import a file" prerequisite stayed hidden because
the visibility rule only checked whether any model existed.

Plan: treat an active import as a prerequisite state even when an older model is still loaded. During
that window, show the import prerequisite with its existing progress label/subtitle and hide the
analysis result/verdict rows until the new scene is attached and analysis can run.

Outcome: `RefreshToolProcessingCards` now uses the active import state when deciding Analysis panel
visibility, and the invalidation path mirrors that same rule. `cmake --build build` passes.

Mini retro: the bug came from two visibility rules drifting apart: "has model" controlled whether the
prerequisite existed, while import progress controlled only its contents. Keeping visibility and
content keyed from the same `importActive` state makes the pending-import UI deterministic.

## Follow-Up: Active Tab Scene Context

Bug: the previous fix still treated import progress as global. If a pending import tab existed, switching
back to an already imported file could still show the pending file's import prerequisite and could dirty
the active scene enough to show an unnecessary analysis progress card.

Plan: make the pending import tab an explicit active file-tab state. The Analysis panel should show the
import prerequisite only when that pending tab is active, and live analysis result rows should be tagged
with the scene that produced them so importing another file does not clear an existing scene's UI.

Outcome: added `pendingImportTabActive` and `analysisUiScene` ownership. Starting an import selects the
pending tab without clearing the current scene's analysis UI; clicking an already-active loaded tab just
leaves pending-import context instead of invalidating geometry. If the pending tab is still active when
import finishes, the new file becomes active; if the user switched back to another file, the import lands
as a background tab.

Mini retro: modeling the active tab directly was cleaner than adding scene pointers to UI primitives.
The UI layer still receives simple visibility flags, while `Display` owns the product rule that pending
imports and loaded scenes are different file-tab contexts. `cmake --build build` passes.

## Follow-Up: Text-Column Progress Underline

Feedback: the Analysis import prerequisite progress bar read as a separate full-card surface, while
the title/subtitle were intentionally indented after the prerequisite checkbox slot. That made the
copy look left-misaligned even though both text lines shared the same content origin.

Plan: keep `accentProgressBar` as the shared progress primitive, but render it as a two-layer underline
aligned to the paragraph text column. The muted track and accent fill stay separate rounded rects; only
their horizontal bounds move from the full card content width to the post-leading-slot text column.

Outcome: `UIRenderer::Render` now draws `accentProgressBar` from the current text origin to the
paragraph's right content edge. For prerequisite rows, that text origin is already advanced past the
leading checkbox, so the title, subtitle, muted track, and accent fill share one left edge. Processing
cards without a leading slot keep the same right edge and now read as underlined text rather than a
separate full-card footer. `cmake --build build` passes.

Mini retro: this was cleaner as a renderer-level adjustment than adding a special import-row flag. The
existing progress primitive already encoded determinate and indeterminate states; moving its bounds kept
the API stable and improved every processing card consistently.

Follow-up: the first underline pass still used the whole text column, so the track extended past the
actual copy. It also did not affect the Files pending-import tab because that tab draws its progress
inside a custom ImGui content callback. Updated the shared renderer to measure the visible title/subtitle
text and use the longest drawn line as the underline width, and updated the Files pending-import tab to
draw the same muted-track/accent-fill underline under its import label.
