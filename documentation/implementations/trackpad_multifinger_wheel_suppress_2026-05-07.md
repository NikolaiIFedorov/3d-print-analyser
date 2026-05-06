# Trackpad vs mouse wheel — multi-finger gate (2026-05-07)

## Problem

Two-finger trackpad scroll generates both `FINGER_*` centroid motion (pan) and `MOUSE_WHEEL` (often duplicate). Unmodified wheel was mapped to zoom/roll like a mouse wheel. `shouldSuppressRedundantTrackpadScroll` only suppressed wheel when `wheel.which == SDL_TOUCH_MOUSEID`, so some OS/SDL paths still delivered zoom/roll during an active two-finger gesture with a non-touch `which`, fighting pan.

## Approach

Treat **active multi-finger trackpad contact** (same predicates as before: `activeTouches`, `sdlHasMultiTouchContact`, drain flags) as “trackpad gesture mode”: suppress **all** unmodified wheel zoom/roll regardless of `which`. Modifier + wheel (orbit / zoom / roll) unchanged.

## Trade-off

If two fingers rest on the trackpad while using an external mouse wheel, unmodified zoom may be suppressed until contacts drop. Acceptable for clarifying trackpad vs mouse per product direction.

## Outcome

`Input::shouldSuppressRedundantTrackpadScroll` returns true for unmodified wheel whenever multi-finger gesture predicates hold, before inspecting `wheel.which`.
