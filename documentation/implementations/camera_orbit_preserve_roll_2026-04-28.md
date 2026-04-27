# Camera orbit — preserve roll, reach plan view (2026-04-28)

## Problem

After tilting the view (often with roll so world +Z reads ~45° on screen), further middle-button orbit could not continue toward a true plan / bird’s-eye view of the XY plane.

## Cause

`Camera::Orbit` updated the eye direction vector `f` correctly, then **always** set `orientation` from `quat_cast(mat3(r,u,f))` built from world +Z and `f`. That basis is the **canonical turntable** with **zero roll** relative to +Z, so **every orbit step erased any user roll** and made combined roll+orbit feel like a hard stop.

## Approach

- Compose orbit as `orientation' = normalize(qPitch * qYaw * orientation)` with the same world yaw axis (+Z) and the same pitch axis `normalize(cross(+Z, fAfterYaw))` as before, so the eye still moves on the same sphere for a given delta.
- When polar angle is inside the safe band, assign `orientation = orientation'` so **roll is preserved**.
- When polar must be clamped (near ±Z), keep the existing horizontal-direction preservation and **basis rebuild** (roll may reset only in that band).

## Outcome

- `Camera::Orbit` now uses `qp * qy * orientation` inside the polar band and only basis-rebuilds when clamping near ±Z.
- `cmake --build build --target CAD_OpenGL` succeeded.

## Mini retro

The old comment warned quaternion drift in roll; the real UX issue was **forced** zero roll every frame. If drift appears after long sessions, re-orthonormalize occasionally or document a reset.
