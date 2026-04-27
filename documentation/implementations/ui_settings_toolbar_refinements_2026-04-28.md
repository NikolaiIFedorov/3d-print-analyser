# UI: settings pills vs persisted state, accent preset, toolbar square hit, panel toggle

## Problem

- Theme (and accent) persisted correctly but segmented controls still showed defaults because `InitUI()` ran before `LoadSettings()` and `Select::activeIndex` was never refreshed.
- Choosing System accent overwrote stored custom H/S with OS accent, so returning to Custom lost the previous choice.
- Toolbar tool rows used a full-width hit/hover rect while row height was smaller, so controls looked non-square.
- Request: clicking the active tool’s toolbar icon should hide that tool’s panel.

## Approach

- Hold `Select*` to appearance theme/accent rows; after `LoadSettings()`, assign `activeIndex` from `themeMode` / `settingsAccentUseSystem` and mark UI dirty.
- On System accent: only call `Color::SetAccent` from `SystemAccent`; leave `settingsAccentHue` / `settingsAccentSat` as the saved custom preset (still written in XML).
- `SectionLine::squareIconHit`: layout/render use a centered `min(rowW, rowH)` square window and centered icon slot for icon-only toolbar lines.
- Toolbar `onClick`: if already on that tool, toggle `uiAnalysis` / `uiCalibrate` visibility; else existing tool switch + `pendingToolSwitch`.

## Files

- `src/display/display.cpp`, `display.hpp`
- `src/display/rendering/UIRenderer/Panel.hpp`, `UIRenderer.cpp`
- `src/display/rendering/UIRenderer/Icons.hpp` — tool glyphs tightened to the ~2s slot (squarer ruler, shorter handle).

## Outcome

Implemented and clean build. Tool switch still forces panel visibility when changing tools; same-tool click toggles panel.

## Mini retro

Keeping a single ordered bootstrap (`InitUI` then `LoadSettings`) is fine if any UI that mirrors persisted state is explicitly synced after load; alternatively building appearance after load would also work but would reorder a large initializer.

## Ship

Committed as `39ccec9` on `imgui-refactor` (amended from earlier hash).

---

## Follow-up (same session, user feedback)

- **Custom accent after System:** `onChange` for Custom (`i == 1`) now calls `Color::SetAccent(settingsAccentHue, settingsAccentSat)` so the swatch-applied values show immediately without opening the picker again.
- **Pan after UI interaction:** `mouseGestures` now treats `ImGui::GetIO().WantCaptureMouse` like `processEvent` does for down, and on **motion** blocks orbit/pan when `WantCaptureMouse || HitTestUI` at the current cursor so drags starting or continuing over ImGui/custom UI do not move the camera.
- **Edge/face z-fighting:** increased `RenderingExperiments::kWireframeClipZNudgeScale` (clip-space Z nudge in `line.vert`) so wireframe lines sit slightly in front of coplanar filled patches in ortho.

---

## Follow-up 2 — ImGui hit + stuck RMB + layout

- **Pan over ImGui:** `io.WantCaptureMouse` can lag one frame; custom `HitTestUI` does not cover Dear ImGui. Added `Display::HitTestImGui()` using `ImGui::FindHoveredWindowEx` (last-frame window rects) and folded it into navigation blocking with mouse and touch.
- **Stuck pan:** `processEvent` skipped `MOUSE_BUTTON_UP` when `WantCaptureMouse`, so `rightMouseDown` / `middleMouseDown` never cleared — split so **button-up always** runs `mouseGestures`.
- **Wheel over UI:** `pendingMouseWheel` path now skips camera zoom/orbit/roll when over ImGui or custom UI or `WantCaptureMouse` (same idea as pan; explains modifier+wheel not firing over widgets).
- **Touch:** two-finger batch pan and one-finger bridge pan/orbit respect the same overlay hit tests.
- **Layout:** Settings column is left of the toolbar (`[Settings][Toolbar][Files…]`).

---

## Follow-up 3 — Pan/wheel, toolbar vs panel, import splash

- **Pan end → roll/zoom:** Trackpads often emit `MOUSE_WHEEL` right after RMB/MMB release or two-finger pan. Ignore **unmodified** wheel-driven camera moves for ~220 ms after those releases and after batched touch pan (`Input::suppressCameraWheelUntilMs`). Modifier + wheel is unchanged.
- **Toolbar “active” vs hidden panel:** `SyncToolbarToolVisualState()` sets each tool row’s `selected` from `activeTool` **and** that tool’s panel `visible`; called after toggling visibility, and from `pendingToolSwitch` in `Render()`.
- **Import progress:** File dialog callback only queues `deferredImportPath`; `Frame()` previously ran a full-screen splash, then **follow-up 4** replaced it with a top status strip + same blocking `CompleteFileImport`. **Feasible** to add a real percentage later by moving parsers to a worker thread and reporting progress — would need thread-safe scene handoff and incremental parsers.

---

## Follow-up 4 — Status strip (replacing splash) + wheel inertia vs finger contact

- **Status strip:** Foreground ImGui draw spans the band above Settings through the toolbar (`DrawForegroundStatusStrip`); idle line from `RefreshStatusStripIdleText` (scene stats); import shows `Importing <file>…` with optional indeterminate/progress later (`statusStripImportProgress01`).
- **Trackpad:** For `SDL_EVENT_MOUSE_WHEEL` with `which == SDL_TOUCH_MOUSEID`, if **no** SDL touch device reports any finger down (`anySdlTouchFingerDown`), skip unmodified camera zoom/roll — treats post-lift inertia as non-user intent. Modifier+wheel unchanged.
- **Note:** With `SDL_HINT_TRACKPAD_IS_TOUCH_ONLY` in `Display::InitWindow`, macOS trackpad exposes finger APIs; separation is coarse (contact vs inertia, multi-finger for pan), not per-resting-finger nuance.

## Outcome (follow-up 4)

Clean build; wheel inertia guard committed separately from earlier UI commits.

---

## Follow-up 5 — Pan lead-in + status strip layering

- **Pan delay:** Two-finger pan ignored `FINGER_MOTION` until per-event `|dx|` or `|dy|` exceeded `kTouchDeadzone` (was 0.0005), which felt like a dead zone before motion. Lowered to `0.00012f`. Removed erroneous `pendingMouseWheel.clear()` from `beginTouchPanAccumForFrame` (wheel queue is drained after processing only).
- **Status strip (superseded by follow-up 6):** Was `ImGui::GetForegroundDrawList()` only; now a real `RootPanel` (see follow-up 6).

---

## Follow-up 6 — Status strip as real `RootPanel`

- Replaced foreground-only draw with a **`RootPanel` id `StatusStrip`**: same GL rounded card + ImGui row as other chrome; anchors `Settings` left → `Toolbar` right, fixed `height` in grid cells.
- **`UIRenderer::ResolveAnchors`**: post-pass shifts `Settings` and `Toolbar` down by strip `rowSpan` + small gap so the strip does not overlap panel headers; re-runs `placeChildrenVertical` for both.
- Content: one `Paragraph` / `SectionLine` with `imguiContent` (text + optional indeterminate pulse during import). `RefreshStatusStripIdleText` toggles `uiStatusStrip->visible` when line empty / scene null.
- **Fix:** `RootPanel::AddParagraph` asserts `children.size() < children.capacity()` — `StatusStrip` must `children.reserve(1)` before the first `AddParagraph` (same pattern as Settings/Toolbar).
- **Layout / look:** Replaced `imguiContent` with synced `SectionLine::text` + `SyncStatusStripTextLine()`: `imguiContent` reserves widget row height (font + frame padding), which exceeded the fixed 0.65-cell `rowSpan` so text drew below the GL card; plain text uses ink metrics and matches the mesh. Dropped fixed height so the strip auto-sizes; smaller `borderRadius` for a flatter status bar (less pill-control). Import indeterminate pulse removed until a second thin row or progress API exists.
- **Match Files chrome:** StatusStrip again uses default `RootPanel` / `Paragraph` margin, padding, and `borderRadius` (same constants as Files). Root panel GL mesh caps corner radius by `min(nominal, 0.5 × shorter side px)` so very shallow panels are not full capsules while tall panels keep the same look.
