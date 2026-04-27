# Camera orbit — dual chart near ±Z pole (2026-04-28)

## Idea

With world +Z as the turntable yaw axis, `Rz(δ)` leaves the eye direction unchanged when `f ≈ ±Z`, so horizontal orbit is singular (“azimuth around Z” is free). Near those poles, switch to an **X chart**: apply horizontal drag as `Rx(δ)` (rotation about world +X) so `f` still moves on the sphere; flip sign for the southern patch so screen motion stays consistent.

## Outcome

- `Camera::Orbit` uses **Rz** for horizontal drag when `|f·Z|` is below the pole threshold, and **Rx** with a sign flip on the southern patch when above.
- `cmake --build build --target CAD_OpenGL` succeeded.

## Mini retro

Naming matches the idea: **azimuthZ** = yaw about +Z away from poles; **azimuthX** = yaw about +X near ±Z where Rz is degenerate on `f`.
