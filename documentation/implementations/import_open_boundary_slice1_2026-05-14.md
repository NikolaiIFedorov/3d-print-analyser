# Import open boundary — slice 1 (implementation)

## Goal

First shipped slice per `import_open_boundary_ux_2026-05-14.md`: after attach, detect `AppInvalidTag::OpenBoundary`, surface **coded** `ToolUserErrorPayload`, add **Fix** (disabled) + **Exit** (dismiss banner only), and gate **import geometry contract** for tools (Calibrate picks, Structure picks) until `OpenBoundary` clears (repair not wired yet → user stays gated or must use a closed model).

## Plan

- `Display::RefreshImportClosedVolumeContractFromScene()` — refresh validity cache per solid if stale; set `calibStepImportClosedVolume` Active/Done; set/clear `importOpenBoundaryToolPayload`.
- New prerequisite row **Closed volume (tools)** (Calibrate + Structure) bound to `calibStepImportClosedVolume`.
- `ImportAllowsGeometryDependentTools()` — both `calibStepImport` and closed-volume step Done for pick gating and Calibrate parameter visibility.
- Calibrate **banner** paragraph: error block + Fix (disabled) + Exit (sets `importOpenBoundaryBannerDismissed`).
- Hook **import-finalize-ui** (activated tab), **file tab** scene switch after `UpdateScene`; reset banner dismiss on new activated import.

## Outcome

Shipped slice 1:

- **`RefreshImportClosedVolumeContractFromScene`** — scans active scene solids (lazy validity cache refresh), sets `calibStepImportClosedVolume`, manages `importOpenBoundaryToolPayload` and resets banner dismiss when the scene is clean or empty.
- **Calibrate + Structure** prerequisite **Closed volume (tools)** (`calibStepImportClosedVolume`).
- **`ImportAllowsGeometryDependentTools`** — gates Calibrate parameter / derived visibility, Structure `GetActivePickFilter`, and Structure footer / optional prereq visibility when the contract is not satisfied.
- **Calibrate banner** — `ToolUserErrorPayload` (`IMPORT_OPEN_BOUNDARY`) + disabled **Fix** (tooltip) + **Exit** (dismiss banner; prerequisite row stays Active until tags clear).
- **Hooks** — import finalize when the imported tab is activated; file-tab switch after `UpdateScene`.

`cmake --build build --target CAD_OpenGL` succeeds.

## Follow-ups

Wire **Fix** to a named repair; 3D blame on boundary edges; call `RefreshImportClosedVolumeContractFromScene` after other topology-changing commits if tools should re-check without a tab switch.

## Mini retro

Validity-to-UI state lives in `Display`; if more tags gate tools similarly, consider a small helper or scene callback to avoid further growth in one file.
