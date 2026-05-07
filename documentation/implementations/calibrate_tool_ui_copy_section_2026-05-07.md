# Calibrate tool — panel copy, section, labels (2026-05-07)

## Goal

- Panel subtitle describes **what** the tool does, not how to pick faces.
- Compensation **result row** is **click-to-copy** (clipboard).
- Clearer **result labels** (e.g. Shrinkage, Hole Radius Offset).
- **Parameters + result** grouped in one collapsible **Measurement** section.

## Approach

- `ToolPanelDef`: optional `parametersSectionTitle` (default `"Parameters"`) so Calibrate can title the parameters section without affecting other tools.
- Calibrate: `flattenParameters = false`, `showSectionHeaders = true`, `parametersSectionTitle = "Measurement"` (renders as uppercase `MEASUREMENT` via `Header`).
- `CalibDrawCopyableResultRow` in `display.cpp` (anonymous namespace): `InvisibleButton` + hover tint + `ImGui::SetClipboardText` for `Label: value`.

## Outcome

- Clean build; behaviour limited to Calibrate panel UI.

## Mini retro

- Copy behaviour is implemented in the Calibrate `imguiContent` path rather than a generic `SectionLine` field, because this row is fully custom-drawn; a shared attachment would only help if more tools adopt the same pattern.
