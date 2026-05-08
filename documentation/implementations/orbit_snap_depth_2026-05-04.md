# Orbit Snap Depth

Date: 2026-05-04

## Idea

Orbit snap should feel like a settle-at-end affordance, not a constraint that keeps the next orbit
stuck to the same plane. The reported depth issue also appeared most strongly while snapped to a
principal plane, where front and back edges overlap on screen and the line shader's forward Z nudge
can make hidden edges win depth.

## Plan

- Keep `Camera::Orbit` as free movement only.
- Add an explicit gesture-end snap so the camera snaps to a nearby principal plane after the orbit
  finishes.
- Wire mouse-button orbit and Alt-wheel orbit through that gesture-end snap.
- Keep the Settings snap slider meaningful by mapping it to the gesture-end snap angle.
- Remove the extra principal-axis wireframe depth nudge so snapped views do not pull back edges
  toward the camera.

## Outcome

- `Camera::FinishOrbitSnap` now applies the principal-axis snap threshold, while `Camera::Orbit`
  no longer latches or holds a snapped orientation during movement.
- Middle-mouse orbit snaps on button release; Alt-wheel orbit snaps once after the wheel batch is
  drained.
- `UserTuning::snap` now derives the snap-in angle directly instead of tuning removed latch
  hysteresis.
- Principal-axis views now use zero additional line Z nudge, relying on normal depth testing rather
  than pulling wire quads forward in the view where hidden back edges are easiest to expose.

Verification: `cmake --build build` passes.

## Mini Retro

Separating free orbit movement from gesture-end snap matched the user-facing behavior more directly
than widening hysteresis. The important review catch was the Snap setting: once the latch went away,
its old exit-angle tuning had to become the settle threshold instead of dead configuration.

## Follow-Up: Grid And Axes Depth

Feedback: after the scene wireframe stopped pulling through faces, the same snapped-view issue was
visible on the reference grid and world axes. Those passes still used a forward depth layer, and the
grid had a second top-down stencil pass that redrew it over solid pixels to recover coplanar floor
lines.

Outcome: grid and axes now use the same clip-Z depth layer as scene geometry instead of a closer
reference-line layer, and the grid no longer draws a second pass on stencil-covered scene pixels.
This favors honest occlusion over showing the floor grid through coplanar/near-coplanar model faces.

Verification: `cmake --build build` passes.

## Follow-Up: Axis/Grid Shimmer

Feedback: the X/Y axes can shimmer against the grid because the grid generator emits the central
`y == 0` and `x == 0` lines, then the axis pass draws colored axes over the same world-space
segments with equal depth.

Plan: keep the existing render order and depth layer, but skip the two redundant central grid lines.
This leaves the colored axes as the only origin-crossing X/Y reference lines and avoids relying on
equal-depth line ties while orbiting or zooming.

Outcome: `ViewportRenderer::Generate` now omits the grid line whose varying coordinate falls within
half a grid spacing of zero for both X-parallel and Y-parallel grid loops. The colored X/Y axes remain
unchanged and no depth-bias tuning was needed.

Verification: `cmake --build build` passes.

Mini retro: removing the duplicate geometry was lower risk than adding another depth-layer constant.
The one review catch was ensuring the skip tolerance follows the current LOD spacing so it remains
stable as the grid spacing changes.

## Follow-Up: Live Orbit Snap, Flaw Framing, Highlight Occlusion

Feedback:

- Analysis flaw rows frame the problem area but keep the old camera orientation, so the flaw can still
  be on a poorly visible side of the model.
- Principal-plane snap only happens after orbit stops; while orbiting, the user has to release before
  the view settles.
- Pick highlights can still look opaque where they are actually behind another face. Hidden portions
  should read through the existing translucent x-ray overlay instead of being pulled forward as solid.

Plan:

- Keep `FrameBounds` as center/zoom only, and add a camera helper that can first orient from a world
  view direction before framing the same bounds.
- Track an orbit snap gesture in `Display`: remember the plane the camera started on, allow live snap
  during `Orbit`, and suppress snapping back to that same plane until the user leaves it.
- Remove the forward polygon offset from pick-highlight fills so the normal pass remains honestly
  depth-tested and the x-ray pass handles occluded selected/hovered portions.

Outcome:

- `Camera` now exposes principal snap axes, snap-axis suppression, and `FrameBoundsFromDirection`.
  Flaw row callbacks still frame the same bounds, but now first orient the view from averaged flaw
  face normals when a reliable direction is available.
- MMB and Alt-wheel orbit start an orbit snap gesture. During orbit, the camera can snap to nearby
  principal planes, while the plane the gesture started on is suppressed until the user moves away
  from it. Newly reached snap planes are also suppressed immediately so continued drag does not feel
  latched.
- Pick highlight fills no longer use polygon offset. The visible portion remains depth-tested, and
  occluded selected/hovered portions are represented by the existing translucent x-ray pass.

Verification: `cmake --build build` passes, and edited files report no linter errors.

## Follow-Up: Face Fill Diagnostic

Feedback: changing x-ray alpha and restoring/removing the x-ray face pass did not materially change the
artifact. The running terminal shows the app was restarted between tests, so this is not just a stale
binary.

Outcome:

- Made `DrawPickHighlight(true)` return immediately and removed the face x-ray call from the render
  path again. Hidden selected geometry is now outline-only.
- This is intentionally diagnostic as well as a possible UX fallback: if the filled-face artifact still
  appears, the source is likely the shared face triangulation / analysis-tinted scene patch rather than
  the pick-highlight x-ray pass.

Verification: `cmake --build build` passes, and edited files report no linter errors.

## Follow-Up: Restore Faint Face X-Ray

Feedback: with filled face x-ray removed, selected faces were no longer visible behind other faces,
while selected edges still were. That made the hidden selection harder to read.

Outcome:

- Restored `RenderPickHighlightXray()` after the main scene pass.
- Kept the x-ray fill depth-fail-only and lowered its alpha to `0.08`; edges remain the stronger
  occluded cue at their existing line alpha. This keeps selected faces faintly visible behind geometry
  without making the fill look as solid as the visible selected surface.

Verification: `cmake --build build` passes, and edited files report no linter errors.

## Follow-Up: Remove Filled Face X-Ray

Feedback: when a selected slanted face sits behind a connected perpendicular face, the filled x-ray
overlay makes the selected face look like it diffracts or bends through the corner.

Outcome:

- Removed the filled face x-ray draw from `Display::Render`.
- Kept the occluded x-ray line pass so hidden selected/hovered forms can still be understood by their
  outline, while the face fill itself remains purely depth-tested and only appears where the selected
  surface is actually visible.

Follow-up: the behavior did not change much, which means the filled x-ray pass was not the root cause.
The pick-highlight fill was still using `uClipZBiasW = 0` while scene patches use the scene mesh clip-Z
bias. That made the highlight live on a different depth layer from the geometry it was meant to test
against. `DrawPickHighlight` now uses `RenderingExperiments::ClipZBiasSceneMeshW()` for the fill pass
so highlighted faces compare against the same depth convention as regular patches.

Verification: `cmake --build build` passes, and edited files report no linter errors.

## Follow-Up: Live Snap Suppression Correction

Feedback: the wider live snap felt like the opposite of plane snap because the gesture began
suppressing the plane it had just snapped into. The selected/highlight cut-through was also still
visible.

Outcome:

- Live orbit snap now suppresses only the plane active at gesture start. Once the camera leaves that
  starting plane, snapping can catch and hold the next nearby principal plane instead of pushing away
  from it on the following mouse event.
- X-ray pick-highlight faces and lines now keep depth testing enabled and draw only when the highlight
  fragment is behind the scene depth (`GL_GREATER`, or `GL_LESS` under reverse-Z). The x-ray pass is
  now specifically an occluded-fragment overlay instead of an always-on overlay for the whole selected
  face.

Verification: `cmake --build build` passes, and edited files report no linter errors.

Mini retro: keeping snap state in `Display` avoided pushing gesture policy into the camera math. The
main trade-off is flaw orientation: averaged normals are a conservative first pass, but flaw clusters
with opposing normals can still fall back to center/zoom only rather than choosing an arbitrary side.

## Follow-Up: Stronger Live Snap And Softer Highlight Bleed

Feedback: the first trial feels close, but live orbit snap should catch principal planes more readily
while navigating. Pick highlights can still occasionally appear as solid through nearer faces, which
means the normal highlight pass needs to be visually forgiving when depth precision is imperfect.

Plan:

- Add a camera snap helper that accepts an explicit angle so live orbit can use a wider catch cone than
  gesture-end snap.
- Keep the same-plane suppression release threshold wider than the live catch cone, otherwise a view
  that just left a snapped plane can immediately re-snap to that same plane.
- Blend the normal pick-highlight fill slightly below full opacity and lower the x-ray fill alpha, so
  occasional depth bleed reads as translucent rather than a solid cut-through.

Outcome:

- Live orbit snap now uses a wider catch angle than release snap, while same-plane suppression clears
  only after moving beyond that live catch cone. This should make principal planes easier to catch
  while dragging without immediately re-snapping to the plane the gesture started on.
- `OpenGLRenderer::DrawPickHighlight` now blends the normal depth-tested fill at partial opacity and
  uses a softer x-ray fill alpha. If depth precision still lets a hidden highlight fragment pass, it
  should now read as translucent instead of a hard opaque cut.

Verification: `cmake --build build` passes, and edited files report no linter errors.
