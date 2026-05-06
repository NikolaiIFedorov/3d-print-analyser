# Calibrate span measurement label (viewport vs UI)

Date: 2026-05-06

## Problem

- Nominal span measurement text on face-hover felt too heavy (too bright / bold).
- It used `ImGui::GetForegroundDrawList()`, which paints above **all** ImGui windows, so it covered tool panels.

## Plan

- Draw the label in a first-submitted fullscreen pass-through ImGui window (`GetWindowDrawList`) so later panel windows stack on top.
- Use body font (Avenir Book) at ~0.92× size, `Color::GetUIText(-1)` (one step dimmer than primary), lighter shadow.

## Outcome

- `Display::Render`: replaced foreground draw list with `##viewport_measure_overlay` window + `AddText` on body font.
- Build: `cmake --build build` OK.

## Mini retro

Foreground draw lists are easy for HUD text but wrong when chrome must stay readable; a no-input root window preserves submission order without capturing input (`NoInputs`).

---

## Follow-up (same theme)

**Issue:** ImGui window layer still drew **above** opaque panel backgrounds because those backgrounds are rasterized with GL in `UIRenderer::Render()` **before** `ImGui_ImplOpenGL3_RenderDrawData`. Any Dear ImGui geometry composites on top of that GL pass, so span labels always floated over side panels.

**Fix:** Draw the span string with `TextRenderer` in `UIRenderer::RenderHudGlyphTextCenteredPx` **before** `UIRenderer::Render()` (same ortho as current `glViewport`). Typography: primary `GetUIText(0)` + small lift (`0.06f`), explicit shadow alpha.

- **Tweak:** `RenderHudGlyphTextCenteredPx` target size `body × 0.80` (fallback 14px); prior `0.94` / 16px read a bit large on retina.
