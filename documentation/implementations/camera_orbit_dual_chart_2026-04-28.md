# Camera orbit — dual chart near ±Z pole (2026-04-28)

## Idea

With world +Z as the turntable yaw axis, `Rz(δ)` leaves the eye direction unchanged when `f ≈ ±Z`, so horizontal orbit is singular (“azimuth around Z” is free). Near those poles, switch to an **X chart**: apply horizontal drag as `Rx(δ)` (rotation about world +X) so `f` still moves on the sphere; flip sign for the southern patch so screen motion stays consistent.

## Outcome

- `Camera::Orbit` uses **Rz** for horizontal drag when `|f·Z|` is below the pole threshold, and **Rx** with a sign flip on the southern patch when above.
- `cmake --build build --target CAD_OpenGL` succeeded.

## Mini retro

Naming matches the idea: **azimuthZ** = yaw about +Z away from poles; **azimuthX** = yaw about +X near ±Z where Rz is degenerate on `f`.

## Follow-up (same day)

Threshold was wrong: `cos(80°)` gates on `f·Z > 0.17`, so almost all “camera above the bed” views used **Rx** for horizontal drag and felt alien. Intended gate is a **small cone** around ±Z (e.g. `f·Z > cos(10°)` ≈ within 10° of straight over/under) where **Rz** truly does almost nothing to `f`.

## Follow-up 2 — dual chart removed

With a **correct** narrow cone (`cos(10°)`), the Rx path almost never runs: until `f` is essentially `(0,0,±1)`, horizontal **Rz** still changes `(f.x, f.y)` and the view spins on screen, so users perceived no benefit. The dual-chart branch was removed; orbit stays **Rz** + pitch + explicit `M_p * M_horizontal * M_ori` + small polar snap.
