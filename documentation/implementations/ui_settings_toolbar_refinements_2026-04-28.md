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
