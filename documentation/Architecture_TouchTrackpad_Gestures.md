# Archived: touch / trackpad two-finger camera navigation

**Status:** removed from the codebase (see git history). This document preserves how it worked and what to restore if you bring it back.

## Why it existed

On macOS, setting `SDL_SetHint(SDL_HINT_TRACKPAD_IS_TOUCH_ONLY, "1")` makes the built-in trackpad report **touch** events (`SDL_EVENT_FINGER_*`) instead of emulating a mouse. That allowed a custom **two-finger** gesture recognizer for **pan**, **pinch zoom**, and **orbit** (asymmetric motion: one finger “anchor”, one “cursor”).

## Why it was removed

- Heuristic classification (ratio of finger speeds, centroid vs finger separation, optional score competition, EMA, streak/lock) was **fragile** and easy to confuse with **normal scroll / pan** expectations.
- **Keyboard and mouse** (RMB pan, MMB orbit, scroll + modifiers) are the primary navigation path; maintaining parallel trackpad logic had poor ROI.
- **Code weight:** `Input` touch history, classifier, and `SessionLogger::LogTrackpadClassify` + large `TrackpadClassifySample` struct.

## SDL setup (historical)

- `SDL_HINT_TRACKPAD_IS_TOUCH_ONLY` = `"1"` — trackpad as touch (also set in `Display::InitWindow` at one point; both were later duplicate).
- `SDL_HINT_MOUSE_TOUCH_EVENTS` = `"0"` and `SDL_HINT_TOUCH_MOUSE_EVENTS` = `"0"` — reduce cross-synthesis between mouse and touch (still used in `Display::InitWindow`).

After removal, the trackpad behaves like a **normal mouse** (move + wheel) for camera control via existing mouse code paths.

## Data structures (historical)

- **`activeTouches`**: `SDL_FingerID → { dx, dy, x, y }` for current frame deltas/positions.
- **`fingerArrivalOrder`**: first two contacts used as the primary pair; extra fingers ignored for classification.
- **`touchHistory`**: per-finger deque of recent `(dx, dy)` samples, size capped (`WINDOW_SIZE`).
- **State machine:** `currentGesture`, `gestureLocked` after `LOCK_FRAMES`, streak counters to avoid one-frame flips, optional EMA on competitive scores.

## Feature pipeline (high level)

1. **Windowed sums** of each finger’s deltas over the last N samples.
2. **Magnitudes** `mag1`, `mag2` → `ratio = minMag / maxMag`. Low ratio → one finger moved more (orbit **geometry**).
3. **Separation / centroid:** current finger vector length vs “start of window” proxy; `d_separation` vs `c_move` to separate pinch vs pan (with thresholds and optional pinch gate so pinch was not classified as orbit).
4. **Competitive scores (later iteration):** `s_pan` / `s_zoom` from share of `c_move` vs `d_separation`; `s_orbit` from asymmetry × pivot term × optional quiet-finger scale; **EMA** to reduce frame noise; orbit only if it **beat** pan/zoom by a margin (and optional pinch gate).
5. **Tie** between pan and zoom: dot product of summed finger vectors.
6. **Apply:** `Display::Pan` (centroid), `Display::Orbit` (dominant finger, Plasticity-style orbit already in `Camera`), `Display::Zoom`.

## Session log (removed)

`SessionLogger::LogTrackpadClassify` wrote `trackpad_gesture` events to `session_log.json` with classifier internals for offline tuning (many float fields, margins, flags).

## Special case (historical)

With touch-only trackpad, **RMB or MMB + one finger** could produce `FINGER_MOTION` instead of `MOUSE_MOTION`. Code bridged that by applying pan/orbit from finger deltas when a mouse button was held. With the hint off, **plain mouse motion** covers RMB/MMB drags again.

## Reintroduction checklist

1. Restore `SDL_HINT_TRACKPAD_IS_TOUCH_ONLY` in `Input` or `Display::InitWindow` (single place).
2. Restore `activeTouches`, `touchHistory`, arrival order, classifier, streak/lock, and `trackpadGestures()`.
3. Re-wire `SDL_EVENT_FINGER_*` in `Input::processEvent` and the mouse-path guard that avoided stealing clicks when gestures were locked.
4. Restore `LogTrackpadClassify` / `TrackpadClassifySample` if you still want JSON tuning, or delete that part and keep a minimal classifier.
5. Re-test **right-click** with two-finger contact on macOS (ordering of FINGER vs MOUSE events).

## Related

- `documentation/Architecture_Input.md` — current mouse/keyboard input (touch section should reference this archive or state “not implemented”).
