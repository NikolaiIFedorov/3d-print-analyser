# Self-intersection import UX + split fix (slice 1)

## Goal

Start the self-intersection invalid-geometry flow with:

- import-time detection surfaced in the same tool-gating contract used by open-boundary,
- a first `Fix` action that attempts split-into-solids repair and groups results as a compound.

## Implemented

- Expanded the import geometry contract in `Display::RefreshImportClosedVolumeContractFromScene()`:
  - mark contract `Active` for either `OpenBoundary` or `SelfIntersection`.
  - when only self-intersection remains, show coded banner payload:
    - code: `IMPORT_SELF_INTERSECTION`
    - message: self-intersection blocks geometry-dependent tools until repair.
- Reused the existing import banner actions:
  - `Fix` now dispatches by payload code:
    - `IMPORT_OPEN_BOUNDARY` -> existing open-boundary fix,
    - `IMPORT_SELF_INTERSECTION` -> new split fix.
- Added `Display::TryFixSelfIntersectionForActiveScene()`:
  - scan solids tagged with `AppInvalidTag::SelfIntersection`,
  - split each candidate solid into face-connected components (edge adjacency),
  - keep first component on original solid, create new solids for the rest,
  - create a compound from split members,
  - refresh caches + UI gating + render invalidations.

## UI copy update

- Prerequisite label changed from `Closed volume (tools)` to `Geometry clean (tools)` in Calibrate and Structure to reflect multi-tag gating (`OpenBoundary` and `SelfIntersection`).

## Notes

- This is intentionally a first split pass; if self-intersection cannot be separated by face connectivity, fix returns failure and keeps the model unchanged.
- Calculation-grid / tolerance-based fallback is deferred to the next slice.
