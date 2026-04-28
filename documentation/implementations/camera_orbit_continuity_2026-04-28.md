# Camera orbit — quaternion + pitch-axis continuity (2026-04-28)

## Problem

Tilting the view (world Z on screen) seemed to stop near ~45°; pushing further made the Z axis **jump** as if the reference changed.

## Likely causes

1. **`glm::quat_cast(mat3)`** returns either **q** or **−q** for the same rotation; picking the wrong hemisphere vs the previous frame yields an apparent **~180°** flip (world axes suddenly “re-point”).
2. **`normalize(cross(Z, f))`** as the pitch axis can **flip sign** when the horizontal part of **f** moves; that inverts the pitch increment for the same mouse motion (**stall** or wrong direction until the next frame).

## Approach

- After `quat_cast(M_new)` and after polar snap `quat_cast(mat3(r,u,f))`, if `dot(q, orientation) < 0`, use **−q** so the quaternion stays in the same **S³ hemisphere** as the previous frame.
- Store **`lastOrbitPitchAxis`**; when applying pitch, if `dot(pitchAxis, last) < 0`, negate **pitchAxis**. Clear the latch on pure-yaw frames, `ResetHomeView`, and `Roll`.

## Outcome

- `cmake --build build --target CAD_OpenGL` succeeded.

## Follow-up — pitch about camera right (screen-vertical orbit)

Quat hemisphere kept. **Pitch axis** changed from `normalize(cross(Z, fAfterYaw))` (turntable latitude tangent) to **`normalize(M_yaw * R_ori * e_x)`** (camera right after horizontal yaw). That aligns vertical mouse drag with **tilt toward/away from plan** when coming from an XZ-style oblique view; the old axis often left colatitude “stuck” around ~45° while the on-screen Z line still moved from mixing yaw/pitch in non-screen axes.

Removed **lastOrbitPitchAxis** continuity (could fight legitimate axis changes); pitch axis is now unambiguous each frame.
