# Calibrate tool — panel copy, section, labels (2026-05-07)

## Goal

- Panel subtitle describes **what** the tool does, not how to pick faces.
- Compensation **result row** is **click-to-copy** (clipboard).
- Clearer **result labels** (e.g. Shrinkage, Hole Radius Offset).
- **Measurement** section (print dimension) and separate **Result** section (compensation row); section headers **not** collapsible for Calibrate.

## Approach

- `ToolPanelDef`: `parametersSectionTitle`, `calculatorSectionTitle`, `sectionHeadersCollapsible`; `Section::collapsibleHeader` in `Panel.hpp`; `BuildToolPanel` / `UIRenderer` respect non-collapsing headers (no chevron, `collapsed` forced false).
- Calibrate: `flattenParameters = false`, `showSectionHeaders = true`, `parametersSectionTitle = "Measurement"`, `hasCalculator` + `calculatorSectionTitle = "Result"`, `sectionHeadersCollapsible = false`.
- `CalibDrawCopyableResultRow` in `display.cpp` (anonymous namespace): `InvisibleButton` + hover tint + `ImGui::SetClipboardText` for `Label: value`.

## Outcome

- Clean build; behaviour limited to Calibrate panel UI.
- Follow-up pass: separate **RESULT** section, non-collapsing section headers, shorter subtitle.
- **Assert fix:** `std::vector` copy into `UIRenderer::AddPanel` does not preserve `capacity()` on empty `children`; `Section::AddParagraph` / `RootPanel::{AddSection,AddParagraph}` now grow capacity when `size >= capacity` instead of asserting.
- Print measurement + result rows use **`pixelImFont` via `ImGui::GetFont()`** inside `imguiContent` (same stack as renderer); idle hit target spans full row width; result clipboard copies **value string only** when present.
- **PARAMETERS** section title restored (`parametersSectionTitle = "Parameters"`).
- **Debug layout:** removed semi-transparent margin “ring” fill so nested section/header/row outlines no longer stack muddy blue fills (comment documents expected edge overlap for outlines).
