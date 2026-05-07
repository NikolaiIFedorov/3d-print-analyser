# Gesture: screen-relative orbit + pan snap floor

Date: 2026-05-08

## Problem / idea

- Pan sometimes felt hard to start; suspected intentional delay (there is none in SDL); axis-snapping on pan could engage on tiny first deltas.
- Orbit horizontal motion used **world +Z** yaw; apparent left/right inverted or swapped depending on camera tilt/roll relative to screen.

## Plan

- Replace `Camera::Orbit` with yaw about camera **up** (screen vertical in camera basis) and pitch about **right after yaw** — pointer-horizontal maps consistently to screen-horizontal orbit at any pose.
- Gate `Display::snapInput` with a small travel threshold so the first few scaled pixels are not axis-snapped into pure H/V lanes.

## Outcome

- `Camera::Orbit` implements screen-relative yaw/pitch as above.
- `Display::snapInput` skips axis alignment until `hypot(x,y) >= kPanSnapTravelFloor` (`display.cpp` anonymous namespace).

Verification: `cmake --build build` passes.

## Mini retro

Screen-relative orbit trades the old fixed **world-Z turntable** yaw for FPS-style consistency; users who relied on “spin like a record on the XY table” from every tilt may notice different paths — acceptable given explicit request for screen-based direction.

`Architecture_Camera.md` orbit/pan text updated to match code (the prior orbit pseudo-code did not match implementation).
