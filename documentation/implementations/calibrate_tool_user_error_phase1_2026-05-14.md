# Calibrate — structured tool user error (phase 1)

## Idea

When Calibrate cannot compute compensation or span, show a **short stable code**, **human message**, and **related parameter label**, with **one-click copy** of `[code] message` for support and logs. Scene highlight is deferred to a later phase.

## Plan

1. Extend `CalibrateCompensation::Values` with optional `const char*` error fields when `valid` is false.
2. In `Display::RefreshCalibCompensation`, set `calibToolError` (optional struct with three strings) for span/workflow failures and forward compensation rejection from `Compute`.
3. In the Calibrate **Result** row (`CalibDerived` imguiContent), render the error block first when `calibToolError` is set; keep a legacy fallback if state is inconsistent.
4. In the **Print measurement** row, emphasize the label when the error’s related label is print measurement.

## Outcome

- **CalibCompensation** now attaches stable `errorCode` / `errorMessage` / `errorParameterLabel` string literals whenever `Compute` rejects (zero measurement, extreme contour ratio, non-finite hole/elephant values).
- **Display::RefreshCalibCompensation** clears or fills `calibToolError` for span geometry, build-axis mismatch, tiny nominal, unsupported workflow pair, and forwards compensation errors; UI dirties when error/valid state changes.
- **Calibrate UI**: Result section shows a **click-to-copy** block (`[CODE] message`) with the **related parameter label** on the line above; **Print measurement** label uses accent emphasis when that label is the related one. Legacy `spanBad` copy row remains only as a fallback if state is inconsistent.

**Build:** `cmake --build build` succeeds (Debug).

## Mini retrospective

- **Worked:** Reusing the existing Calibrate Result `imguiContent` avoided layout-system changes for multi-line parameter rows while still delivering copyable text and parameter association.
- **Follow-up (later phases):** Scene highlight and/or a shared `ToolUserError` type for non-Calibrate tools; optional `getMinContentHeightPx` on `SectionLine` if errors should sit directly under a parameter row without using the Result block.
