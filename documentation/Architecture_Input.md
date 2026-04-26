# Architecture: Input — Event Handling

## Executive Summary

The `Input` module translates SDL3 events into camera operations and application commands. It handles **mouse** input (right/middle drag for pan/orbit, scroll wheel with modifiers) and routes events through ImGui. **Multi-finger trackpad / touch** camera gestures were **removed** to keep the code small and avoid conflicts with key-based navigation; an archive of the old design is in `documentation/Architecture_TouchTrackpad_Gestures.md`.

`FileImport` (native file dialog) lives alongside this module but in separate files.

**Verdict: Approve** (simpler input path, same Display API)

**Top strengths:**
1. Small surface area — mouse + wheel + UI hit-test only.
2. Input only calls `Display` public methods; no scene/rendering coupling.
3. No `SDL_HINT_TRACKPAD_IS_TOUCH_ONLY` — trackpad uses normal mouse/wheel behavior on typical desktop setups.

**Concerns / follow-ups:**
1. `Input` still depends on concrete `Display*` (acceptable at current scale).
2. Touchscreen-only users get no custom touch navigation until a future opt-in (see archive doc).

---

## Requirements (current)

- **Mouse:** middle drag → orbit, right drag → pan, scroll → roll (horizontal) + zoom (vertical) with optional Alt / Shift / Ctrl modifiers.
- **UI:** `HitTestUI` before starting viewport drags; ImGui captures mouse/keyboard when appropriate.
- **Window:** resize, system theme, debug keys (`\``, `/` for bug marker).
- **Not in scope:** two-finger trackpad pan/zoom/orbit (removed).

Trackpad two-finger scroll is handled as **mouse wheel** events, not as a custom classifier.

---

## Data flow (simplified)

```
SDL_WaitEventTimeout / SDL_PollEvent
├── SDL_EVENT_QUIT → exit
├── SDL_EVENT_WINDOW_* → aspect, theme, focus (release mouse buttons on focus loss)
├── SDL_EVENT_KEY_DOWN → debug / bug marker (if not captured by ImGui)
├── SDL_EVENT_MOUSE_WHEEL → mouseGestures (modifiers or bare roll+zoom)
└── SDL_EVENT_MOUSE_BUTTON_* / MOTION → mouseGestures (if not ImGui capture)
```

---

## File layout

| Component | Files | Role |
|-----------|-------|------|
| **Input** | `Input.hpp`, `Input.cpp` | Event loop, mouse/wheel, `syncWindowRelativeMouseMode` for RMB/MMB drags |
| **FileImport** | `FileImport.hpp`, `FileImport.cpp` | SDL3 native open dialog |

---

## Design notes

- **Relative mouse mode** is enabled while right or middle button is held (after UI hit-test), for correct drag behavior.
- **Scroll:** Alt = orbit, Shift = zoom, Ctrl = roll (x); unmodified = roll (x) + zoom (y) from wheel.
- **Session logging:** `SessionLogger` no longer records per-frame trackpad classification (removed with gestures).

---

## Technology

| Library | Use |
|---------|-----|
| SDL3 | Events, window |
| ImGui | `WantCaptureMouse` / `WantCaptureKeyboard` |
| GLM | Via `Display::ScreenToWorld` for zoom-to-cursor paths |

---

## History

- **Trackpad as touch + `classifyTwoFinger`:** removed. See `documentation/Architecture_TouchTrackpad_Gestures.md` for recovery notes and the old algorithm outline.
- **`SDL_HINT_TRACKPAD_IS_TOUCH_ONLY`:** was set in `Input` and `Display::InitWindow`; both removed. `Display` still sets `SDL_HINT_MOUSE_TOUCH_EVENTS` / `TOUCH_MOUSE_EVENTS` to `0` to limit cross-wiring when touch devices are present.

---

## Future

- **Keyboard shortcuts** (frame view, numpad views) can be added in `SDL_EVENT_KEY_DOWN`.
- **Optional touch / trackpad** mode: reintroduce using the archive doc, ideally behind a setting.
