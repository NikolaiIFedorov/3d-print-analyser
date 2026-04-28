# Viewport orbit/pan — continue drag over UI (2026-04-28)

## Problem

RMB/MMB navigation stopped when the cursor moved over ImGui or custom UI: motion was gated on `WantCaptureMouse` / `HitTestUI` / `HitTestImGui` every frame, and `processEvent` dropped `MOUSE_MOTION` entirely when ImGui wanted capture.

## Approach

- **`MOUSE_MOTION`:** If `middleMouseDown || rightMouseDown`, do not treat UI hit-tests or `WantCaptureMouse` as blocking orbit/pan (drag must have started on the viewport because mouse-down already required a clear viewport).
- **`processEvent`:** Deliver `MOUSE_MOTION` to `mouseGestures` when either ImGui does not want capture **or** a viewport nav button is held.
- **Touch bridge** (`FINGER_MOTION` with one contact + RMB/MMB): same idea — skip the old UI block for that path; allow `FINGER_MOTION` through `WantCaptureMouse` when a nav mouse button is down.

## Outcome

`cmake --build build --target CAD_OpenGL` succeeded.
