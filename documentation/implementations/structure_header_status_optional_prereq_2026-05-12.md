# Structure panel: header status text + optional prerequisite rows

## Problem / idea

- CGAL failures in the Structure carve path can terminate the process (assertions) or surface only as log lines; the user wants a short **status / error string to the right of the panel title** (Structure).
- The tool should document **optional scene-driven steps** (e.g. face exclusions) with the same card look as prerequisites but **without** a checkbox.

## Plan

1. Extend `Header` with optional `trailingCaption` + typography fields; layout inflation + foreground draw in `UIRenderer`.
2. Extend `ToolPanelDef` with `optionalPrerequisites` and emit a dedicated `OptionalPrerequisites` section built via `BuildPrerequisiteParagraph` with no `leadingDraw`.
3. `Display::BeginStructureStagingSession`: aggregate carve failures / exceptions into the Structure header trailing text; clear on restore/commit/finalize and at session start.
4. Wrap `TryApplyStructureCarve` in `try/catch` for `std::exception` and `...` so thrown CGAL errors become a returned message instead of crashing when exceptions are enabled.

## Notes

- CGAL **assertion failures** that call `abort()` are still not recoverable in-process; exception coverage helps when CGAL is built / configured to throw.

## Outcome

- **Header trailing text:** `Header` gained `trailingCaption`, `trailingTextDepth`, `trailingFontScale`. `UIRenderer` inflates the header paragraph box when trailing text is present and draws it right-aligned on the title row (foreground draw list). Structure staging sets this from carve failures (including `std::exception` from CGAL when exceptions are enabled) and clears it on restore/commit/session start.
- **Optional prerequisite rows:** `ToolPanelDef::optionalPrerequisites` adds an `OptionalPrerequisites` section built with the same `BuildPrerequisiteParagraph` as prerequisites but without `leadingDraw`. Structure panel includes one row explaining optional face exclusions; visibility mirrors scene-edit mode (`activeHasModel`).
- **CGAL throws:** `TryApplyStructureCarve` wraps the carve body in `try/catch` so thrown errors become a false return with `errOut` instead of unwinding uncaught. **Assertion / abort paths** in CGAL are unchanged.

## Mini retro

- Reused prerequisite paragraph builder for “optional prerequisites” to avoid a parallel card style.
- Header trailing is generic on `Header` so section headers could use it later without new UI types.
- Follow-up if needed: map `structureExcludedFaces` across `Scene::Clone` so exclusions affect staging carve, not only pre-staging preview.
