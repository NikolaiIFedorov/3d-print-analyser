# Structure: skip staging mode when every carve attempt fails (2026-05-13)

## Problem

After CGAL/structure carve failed for all solids, the app still swapped in the staging scene and set `structureOriginalScene`, so `IsStructureStagingActive()` was true even though geometry was unchanged. That blocked starting a new staging session (`BeginStructureStagingSession` returns early), stalled analysis/rebuild expectations, and correlated with bad quit UX.

## Approach

In `Display::PollStructureStagingTaskIfReady`, when `carveAttempts > 0` and `carvedSolids == 0`, treat as a **complete failure**: show the error line on the Structure panel, clear structure bake caches, refresh UI/pick — **do not** move `ownedScenes` / `structureOriginalScene`. Drop the worker’s staging `unique_ptr` (same geometry as pre-carve clone).

Partial success (`carvedSolids > 0` but some failures) is unchanged: staging swap + “Partial:” messaging.

## Outcome

- `src/display/display.cpp`: `PollStructureStagingTaskIfReady` — if `carveAttempts > 0 && carvedSolids == 0`, skip staging swap; show `firstErr` or a generic message; clear bake cache and refresh pick/UI.
- Clean build: `cmake --build build --target CAD_OpenGL` succeeded.

Pending in-app validation: self-intersecting inset repro — expect error text, no staging lock, analysis and re-enter Structure behave normally.

## Mini retro

Focused guard at apply site avoids new state flags; partial carve path untouched. If we ever need “failed staging” UX (e.g. diff failed solids), we can revisit without re-blocking `BeginStructureStagingSession`.
