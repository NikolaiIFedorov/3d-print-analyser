# Length units (UI + settings) — 2026-05-04

## Idea

- Canonical world/analysis values stay in **millimeters** (existing convention).
- Users choose a **default length unit** (mm, cm, in, ft) in Settings; bare numbers use that unit.
- Typed values may append an explicit suffix (`5mm`, `3.5in`, …); decimal separator is `.` only (comma rejected in parser).
- UI controls for distances show values in the default unit and commit **mm** to the model.

## Plan

1. Add `include/LengthUnit.hpp` — conversion, abbreviations, `TryParseLengthToMm`, display formatting.
2. Extend `Settings` with `defaultLengthUnit` (int 0–3); persist in XML.
3. **Viewport** pill selector + length-aware grid row (`makeSettingsLengthDrag`).
4. **Analysis** thin/small threshold rows: length mode with drag + click-to-text (`InputText`) for suffixed parse.
5. **Calibrate** print measurement: `InputText` + parse; overlay shows formatted value in default unit.

## Outcome

Implemented as planned. Build: `cmake --build build` OK (AppleClang).

## Mini retro

- **Worked:** Centralizing parse/conversion in one header keeps `display.cpp` lambdas readable enough; reusing the same drag+text pattern as settings for flaw rows avoids a separate widget module for now.
- **Watch:** ImGui id discipline for `InputText` vs `DragFloat` must stay unique per row (`##ltxt` / `##slen` prefixes). Future length fields should reuse `makeSettingsLengthDrag` or a small shared helper to avoid drift.

## Update 2026-05-04 — unit inside the control

- Readouts and unit hints use **`paramZoneRight`** (right edge of the actual `SetNextItemWidth` zone) instead of the full row width so text sits inside the same rounded frame as the widget.
- **Click-to-edit** (`InputText`): buffer is **`"<num> <abbr>"`** so the suffix is literally in the field; overlay suffix skipped in that mode.
- **Keyboard edit on DragFloat** (length): item width is reduced slightly and the abbreviation is drawn at **`paramZoneRight`** inside the reserved strip (same frame).
- **Calibrate** idle readout: single formatted string right-aligned to **`GetItemRectMax().x`** (no duplicate trailing abbr).

## Update — calibrate crash + right-anchored length fields

- **Calibrate:** Removed `*pmEditing = IsItemActive() && WantTextInput` after `InputText`, which could fight click-to-focus and led to bad state / crashes on deactivate. Edit mode is now: open on `IsItemClicked()` while idle, close only on `IsItemDeactivatedAfterEdit()` after parsing.
- **Analysis / settings length:** DragFloat / InputText cursor is **`paramZoneRight - itemW`** so the control stays **right-aligned** in the param band when width toggles between full and suffix-reserved (no leftward jump when leaving keyboard edit).
- **Calibrate** overlay: guard **null `ImFont`** before `CalcTextSizeA`.
