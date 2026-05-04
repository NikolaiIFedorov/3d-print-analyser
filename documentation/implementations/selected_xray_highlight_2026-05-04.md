# Selected X-Ray Highlight

Date: 2026-05-04

## Idea

Selected calibration faces/edges should remain understandable when another form occludes them. Keep the
normal depth-tested highlight for visible geometry, then draw a faint always-visible overlay so hidden
parts read as selected without fully replacing the front object.

## Plan

- Reuse the existing pick-highlight face and line buffers; do not add another selection data model.
- Add an alpha-capable x-ray draw mode that disables depth testing and uses blending for the overlay pass.
- Render the x-ray pass after the scene/grid/axes so selected forms remain visible through the viewport
  reference geometry too.

## Outcome

- Added `uAlpha` to the shared scene and line shaders, with opaque defaults set by normal scene,
  wireframe, grid, and axes passes.
- Added x-ray variants for pick-highlight faces and lines. They reuse the existing selected/hover pick
  buffers, disable depth testing, enable alpha blending, and restore GL state afterward.
- Render now keeps the normal depth-tested highlight first, then draws a faint always-visible fill and a
  slimmer always-visible line overlay after grid/axes.

Verification: `cmake --build build` passes, and the edited files report no linter errors.

## Mini Retro

Reusing the pick-highlight buffers kept the trial small and avoided inventing a second selection model.
The main trade-off is that the overlay currently follows the combined pick-highlight buffer, so hover
geometry receives the same faint x-ray pass as committed calibration picks. If that feels noisy, split
the selected and hover highlight buffers before tuning colors further.

## Follow-Up: Do Not X-Ray Hover Candidates

Feedback: the first pass made hidden hover candidates visible/selectable, which is unnecessary while
picking. Keep x-ray behavior only for forms the user has already committed as calibration selections.

Outcome: the pick-highlight upload now records how many leading indices belong to committed
calibration picks. Normal highlight draws the full buffer (selected + hover), while the x-ray pass draws
only that selected prefix. This keeps occluded selected forms visible without using the x-ray overlay as
a discovery mechanism for hidden hover targets.

## Follow-Up: Hover-Confirmed X-Ray

Feedback: when a visible part of a form is hovered, showing the rest of that same form through occluders
is useful. This should not reintroduce hidden-object discovery; the hover must still come from the normal
depth-tested pick path first.

Outcome: after committed picks are appended, the hover face/edge is appended to the same highlight buffer
and the x-ray draw count now includes that hover range. The x-ray pass still cannot create a hover by
itself; it only reveals the rest of the current visible hover target.

## Follow-Up: Face x-ray fill (behind-occluders)

Face pick-highlight x-ray was stubbed (`DrawPickHighlight(true)` returned immediately) because mesh
tessellation can make translucent fills appear to leak across coplanar neighbors. Approach (1): keep the
opaque depth-tested fill pass as-is for visible geometry; restore the faint face pass that uses
`DepthCompareBehind` (same as line x-ray) plus blend, drawn before the slimmer line x-ray so outlines
remain the strongest cue.

Outcome: `DrawPickHighlight(true)` draws `pickHighlightXrayIndexCount` indices; `Display::Render` calls
`RenderPickHighlightXray()` then `RenderPickHighlightLinesXray()`.

Tuning (2026-05-04): Occluded-face alpha raised from implicit `0.08` → `RenderingExperiments::kPickHighlightFaceXrayAlpha` (`0.14`) after feedback that the fill read too faint; single knob for rebuild-less tweaks.

