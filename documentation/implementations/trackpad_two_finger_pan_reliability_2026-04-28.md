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

---

## 2026-05-06 — Jitter + mouse wheel blocked while trackpad contacts linger

### Problem

- Two-finger pan could feel **jittery** (per-finger `tf.dx`/`tf.dy` averaged over events; asymmetric finger updates/noise).
- **Physical mouse wheel** zoom sometimes did nothing until a click/other gesture; **trackpad** zoom still worked. `shouldSuppressRedundantTrackpadScroll` treated `activeTouches >= 2` / `sdlHasMultiTouchContact()` as reason to drop **all** wheel events, including real mouse wheels, while SDL still reported multi-contact.

### Approach

- Drive batched two-finger pan from **centroid displacement** between start-of-pass and end-of-pass normalized positions; `ensureTouchPanCentroidAnchor()` when the second finger arrives mid-queue.
- Suppress redundant scroll only for **`SDL_TOUCH_MOUSEID`** when multi-contact pan is active; never suppress non-touch mouse wheel `which`.

### Files

- `src/input/Input.cpp`, `Input.hpp`

### Outcome

Clean `cmake --build build --target CAD_OpenGL`. Note: unmodified touch-derived wheel is no longer globally suppressed—only when multi-contact pan would duplicate; single-finger trackpad scroll may now reach zoom/roll unless blocked by inertia / UI gates.

---

## 2026-05-06 — Duplicate wheel roll/zoom during two-finger pan

### Problem

Two-finger trackpad pan often also drove **roll** (horizontal wheel) and **zoom** (vertical wheel) because duplicate `SDL_EVENT_MOUSE_WHEEL` events (`SDL_TOUCH_MOUSEID`) were not always suppressed when multi-touch was momentarily inconsistent inside one event drain.

### Approach

- Track **`multiFingerSeenThisEventDrain`**: set when `activeTouches.size() >= 2` or `sdlHasMultiTouchContact()` while handling finger events in the current `handleEvents` drain (cleared at the start of each drain).
- Extend **`shouldSuppressRedundantTrackpadScroll`** to treat that latch like multi-contact for suppression (still only `SDL_TOUCH_MOUSEID`, still never suppress physical mouse wheel `which`).

### Files

- `src/input/Input.cpp`, `Input.hpp`

### Outcome

`cmake --build build --target CAD_OpenGL` succeeded. Needs runtime confirmation on macOS trackpad (two-finger pan vs roll/zoom bleed).
