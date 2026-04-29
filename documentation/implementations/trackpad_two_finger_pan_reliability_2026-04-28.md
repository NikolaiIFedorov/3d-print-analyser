# Trackpad two-finger pan — cutoff and startup delay (2026-04-28)

## Problem

Intermittent pan **cutoffs** (long drags, direction changes) and a **delay** before pan starts after placing two fingers.

## Root causes (code)

1. **`applyBatchedTwoFingerPan`** aborted if **any** active touch pixel hit custom UI or ImGui (`WantCaptureMouse` OR per-finger hit). A two-finger span often leaves one contact over chrome while the other stays on the viewport → whole batch dropped.

2. **`FINGER_UP` / `FINGER_DOWN` / `FINGER_CANCELED`** were skipped when `io.WantCaptureMouse`, so touch counts could desync from hardware (stale ≥2 contacts or missing contacts) → wrong branch or missed pan.

3. **Per-event deadzone** required `|dx| >= k OR |dy| >= k`. Near direction reversals, many events have **both** components small → events dropped before accumulation → stutter/cutoff feel.

## Plan

- Hit-test the **centroid** of active touches once (same gates: ImGui capture + UI + ImGui window at that point).
- Always update **`activeTouches` / `fingerArrivalOrder`** on finger down/up/cancel regardless of `WantCaptureMouse`; keep `WantCaptureMouse` on **FINGER_MOTION** for applying camera deltas.
- Gate per-event motion with **`hypot(dx,dy) >= k`** (slightly more diagonal-friendly) and **lower `k`** a bit so small coherent motion still registers.

## Outcome

- Centroid hit-test, unconditional finger up/down/cancel tracking, `hypot` motion gate + halved `kTouchDeadzone`.
- Clean `cmake --build build` succeeded.

## Mini retro

Hit-test-any-finger was the main surprise for multi-touch; centroid is a cheap fix. Touch state must stay aligned with SDL even when ImGui captures mouse.
