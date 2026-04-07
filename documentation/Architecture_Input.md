# Architecture: Input — Event Handling & Gesture Recognition

## Executive Summary

The Input module translates raw SDL3 events into high-level camera operations and application commands. It consists of two classes: `Input` (gesture recognition and event dispatch) and `FileImport` (native file dialog via SDL3). The module handles mouse input (orbit, pan, zoom via right/middle buttons and scroll wheel), multi-touch trackpad gestures (two-finger pan, zoom, orbit classification), and keyboard modifiers. It is ~350 lines across 4 files.

**Verdict: Approve**

**Top 3 strengths:**
1. Sophisticated trackpad gesture classifier that distinguishes pan, zoom, and orbit from two-finger input using motion history and directional analysis.
2. Clean separation — Input only calls `Display` public methods, never reaches into Scene or rendering internals.
3. Gesture locking mechanism prevents mid-gesture reclassification, avoiding jittery transitions.

**Top 3 concerns:**
1. Input depends directly on `Display*` — would benefit from an abstract interface for testability.
2. Duplicate `SDL_SetHint` call — `SDL_HINT_TRACKPAD_IS_TOUCH_ONLY` is set in both `Input::Input()` and `Display::InitWindow()`.
3. No keyboard shortcut system — modifier keys are checked inline in gesture handlers.

---

## 1. Requirements & Motivation

### Functional Requirements
- Translate mouse events into 3D viewport operations: orbit (middle-click drag), pan (right-click drag), zoom (scroll wheel).
- Classify macOS trackpad two-finger gestures as pan, zoom, or orbit based on finger motion patterns.
- Support modifier keys: Alt+scroll for orbit, Shift+scroll for zoom, bare scroll for roll+zoom.
- Open native file dialogs for model import (STL, STEP, 3MF, OBJ, PLY).
- Handle window resize events.
- Route UI clicks to the UI hit-test system before processing 3D viewport interactions.

### Non-Functional Requirements
- Responsive gesture detection — classify within ~10 frames (LOCK_FRAMES).
- No false classifications — use a sliding window to smooth noisy touch input.
- macOS trackpad compatibility as primary target.

### Constraints
- SDL3 event API (touch events for trackpad, mouse events for external mouse).
- Single-threaded event loop.

---

## 2. Solution Description

### Components

| Component | Files | Responsibility |
|-----------|-------|----------------|
| **Input** | `Input.hpp/cpp` | SDL3 event loop. Mouse gesture handling. Trackpad two-finger gesture classification (pan/zoom/orbit). Keyboard modifier detection. UI hit-testing delegation. |
| **FileImport** | `FileImport.hpp/cpp` | Opens SDL3 native file dialog with CAD file filters. Invokes callback with selected file path. |

### Data Flow

```
SDL_PollEvent()
├── SDL_EVENT_QUIT → return false (exit loop)
├── SDL_EVENT_WINDOW_RESIZED → Display::SetAspectRatio()
│
├── Mouse events → mouseGestures()
│   ├── SDL_EVENT_MOUSE_WHEEL
│   │   ├── Alt held    → Display::Orbit()
│   │   ├── Shift held  → Display::Zoom()
│   │   └── No modifier → Display::Roll() + Display::Zoom()
│   ├── SDL_EVENT_MOUSE_BUTTON_DOWN
│   │   ├── Left   → Display::HandleClick()  (UI first)
│   │   ├── Right  → set rightMouseDown (if not over UI)
│   │   └── Middle → set middleMouseDown (if not over UI)
│   └── SDL_EVENT_MOUSE_MOTION
│       ├── middleMouseDown → Display::Orbit()
│       └── rightMouseDown  → Display::Pan()
│
├── Touch events → activeTouches map + touchHistory
│   ├── SDL_EVENT_FINGER_DOWN  → register touch
│   ├── SDL_EVENT_FINGER_UP    → unregister, reset if < 2
│   ├── SDL_EVENT_FINGER_MOTION
│   │   └── trackpadGestures()
│   │       ├── classifyTwoFinger() → Pan / Zoom / Orbit
│   │       ├── Pan   → Display::Pan(average dx, dy)
│   │       ├── Orbit → Display::Orbit(moving finger dx, dy)
│   │       └── Zoom  → Display::Zoom(combined magnitude)
│   └── SDL_EVENT_FINGER_CANCELED → clear all state
```

### Gesture Classification Algorithm

The two-finger gesture classifier (`classifyTwoFinger()`) works as follows:

1. **Accumulate history**: Each finger's `(dx, dy)` deltas are stored in a sliding deque of size `WINDOW_SIZE` (8 frames).
2. **Compute per-finger motion vectors**: Sum the last 8 deltas for each finger → `(sum1dx, sum1dy)`, `(sum2dx, sum2dy)`.
3. **Magnitude ratio test**: `ratio = minMag / maxMag`. If `ratio < 0.3`, one finger is stationary → **Orbit** (only the moving finger drives rotation).
4. **Direction test**: If ratio ≥ 0.3, compute `dot(finger1, finger2)`. If dot ≥ 0, fingers move in the same direction → **Pan**. If dot < 0, fingers move in opposite directions → **Zoom**.
5. **Lock**: After `LOCK_FRAMES` (10) frames of consistent classification, the gesture type is locked and won't change until both fingers lift.

For orbit, the **moving finger** is identified by comparing accumulated historical displacement magnitudes — the finger with more total motion controls the rotation.

### Key Abstractions

- **`GestureType` enum**: `None`, `Pan`, `Zoom`, `Orbit` — clean state machine for gesture classification.
- **`TouchAccum` + `touchHistory`**: Per-finger sliding window for smoothing noisy trackpad input.
- **`activeTouches` map**: `SDL_FingerID → Touch` for real-time delta tracking.
- **UI hit-testing**: Left clicks and right/middle presses first check `Display::HitTestUI()` before routing to viewport interactions.

---

## 3. Design Principles

### Conforming

| Principle | Evidence |
|-----------|----------|
| **SRP** | `Input` handles event dispatch only; `FileImport` handles file dialogs only. Neither touches scene or rendering. |
| **Encapsulation** | Gesture state (`activeTouches`, `touchHistory`, `currentGesture`) is private. Only `handleEvents()` is public. |
| **Consistency** | Mouse sensitivity, deadzone, and window size are `static constexpr` members, not magic numbers. |

### Deliberately Relaxed

| Principle | Deviation | Justification |
|-----------|-----------|---------------|
| **DIP** | `Input` depends on concrete `Display*` | Adding an abstract `IViewportController` interface would be overengineering for a single implementation. |
| **OCP** | Mouse/trackpad gestures are hardcoded switch statements | Gesture mapping is unlikely to change; a strategy pattern would add complexity without benefit. |

---

## 4. Alternatives Considered

| Decision | Current Approach | Alternative | Verdict |
|----------|-----------------|-------------|---------|
| Gesture classification | Sliding-window motion analysis | Fixed two-finger distance delta (pinch-only zoom) | Current approach is more flexible — handles asymmetric finger movement for orbit. **Current is better.** |
| Orbit control | One stationary + one moving finger | Both fingers dragging in arc | Current matches CAD conventions where one finger is an anchor point. |
| File dialog | SDL3 `SDL_ShowOpenFileDialog` | Native NSOpenPanel / IFileDialog | SDL3's dialog is cross-platform and sufficient. **Correct choice.** |
| Scroll → camera | Scroll Y = zoom, Scroll X = roll | Scroll = orbit | Zoom on scroll wheel is the most common convention. |

---

## 5. Technology & Dependencies

| Library | Role in Input |
|---------|--------------|
| SDL3 | Event polling (`SDL_PollEvent`), touch events, mouse events, key modifiers, file dialogs |
| GLM | `glm::vec3` for cursor position passed to `Zoom()` |

No new dependencies.

---

## 6. Tradeoffs

| Decision | Chosen Approach | What's Sacrificed | Rationale |
|----------|----------------|-------------------|-----------|
| Touch-as-trackpad hint | `SDL_HINT_TRACKPAD_IS_TOUCH_ONLY = "1"` | Direct touch-screen support | macOS is primary target; trackpad gestures need touch events without mouse emulation. |
| Gesture lock after 10 frames | Stability | Responsiveness | Prevents jittery re-classification mid-gesture. 10 frames ≈ 160ms at 60fps — acceptable latency. |
| Motion deadzone (0.0005) | Noise rejection | Precision for very slow movements | Trackpad input is noisy; deadzone prevents stationary-finger drift from affecting classification. |
| Mouse sensitivity constant (0.005) | Consistent feel | User customization | Hardcoded but reasonable. |

---

## 7. Best Practices Compliance

### Conforming
- **Constants**: All magic numbers extracted to `static constexpr` members (`MOUSE_SENSITIVITY`, `LOCK_FRAMES`, `WINDOW_SIZE`, `ORBIT_RATIO_THRESHOLD`, `TOUCH_DEADZONE`).
- **State cleanup**: `resetGestureState()` clears all gesture bookkeeping on finger-up / cancel events.
- **UI precedence**: Left clicks route through `Display::HandleClick()` (UI hit-test) first. Right/middle clicks check `HitTestUI()` before enabling viewport drag.
- **FileImport callback cleanup**: `FileCallback*` allocated with `new` is properly `delete`d inside the SDL dialog callback — no leak path.

### Non-Conforming

| Issue | Severity | Details | Remediation |
|-------|----------|---------|-------------|
| Duplicate `SDL_SetHint` | Minor | `SDL_HINT_TRACKPAD_IS_TOUCH_ONLY` set in both `Input::Input()` and `Display::InitWindow()` | Remove from one location (preferably keep in `Display` since it's window-related) |
| `FileImport` uses raw `new` for callback | Minor | `auto *cb = new FileCallback(...)` followed by `delete callback` in SDL callback | Could use `SDL_SetPointerProperty` or ensure exception safety, but current pattern is correct for SDL's C-callback API |
| `snapInput` modifies by reference | Minor | `Display::snapInput(float&, float&)` is called from `Input` via `Display::Orbit/Pan` — but it's in `Display`, not `Input` | `snapInput` belongs in `Input` since it's an input-processing concern |
| No keyboard shortcut system | Minor | Modifier checks are inline in `mouseGestures()` | Acceptable at this scale; extract if more shortcuts are added |
| `Zoom` cursor position unused | Minor | `Display::Zoom()` receives `glm::vec3(0,0,0)` from most trackpad/scroll paths instead of actual cursor position | The cursor position enables zoom-to-cursor; pass actual screen-to-world projected cursor |

---

## 8. Risks & Future-Proofing

### Risks

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| Cross-platform trackpad differences | Medium — gesture classification tuned for macOS | Medium (if Linux/Windows added) | Test on other platforms; may need platform-specific thresholds |
| Touch screen devices | Low — trackpad hint disables touch-screen | Low (desktop CAD app) | Add a runtime toggle for touch-screen mode if needed |

### Future Considerations
- **Keyboard shortcuts**: Adding hotkeys (e.g., `F` to frame, `1-6` for standard views) would be straightforward in the `SDL_EVENT_KEY_DOWN` handler.
- **Undo/Redo**: Input would dispatch undo commands (Cmd+Z / Ctrl+Z) to a command stack.
- **Selection**: Click-to-select would need GPU picking or ray-casting — Input would dispatch click position to a selection manager.
- **Configurable input mapping**: Allow users to remap mouse buttons and modifier keys.

---

## 9. Recommendations

### Must-Have
_(None — module works correctly as-is.)_

### Should-Have
1. **Remove duplicate `SDL_SetHint`** — keep it in `Display::InitWindow()` only.
2. **Pass cursor position to Zoom** — enable zoom-to-cursor by projecting the mouse/touch position to world coordinates.

### Nice-to-Have
3. **Move `snapInput` to Input** — it's an input-processing function, not a display concern.
4. **Abstract display interface** — replace `Display*` with `IViewportController*` if unit testing input logic becomes a goal.
5. **Configurable sensitivity** — expose mouse sensitivity and gesture thresholds as user settings.
