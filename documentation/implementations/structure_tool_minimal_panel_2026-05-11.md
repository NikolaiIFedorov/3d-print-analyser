# Structure tool — minimal panel (2026-05-11)

## Idea

Structure preview tuning is mostly file-invariant; remove per-tool ImGui controls while keeping diamond (`BuildAdjacentFaceMidpoints`) and inset face loops with the same visualization (including translucent shell default).

## Plan

- `Display`: drop preview/rib/pattern toggles and rib float params; keep inset mm / depth / full-depth defaults and translucent shell default on.
- `RefreshStructurePreviewForRenderer`: when Structure tool panel is visible and scene has solids, always fill main preview + inset segments; clear rib segments.
- `BuildToolPanel`: Structure `parameters` empty; subtitle explains behavior.

## Outcome

Implemented in `src/display/display.cpp` and `src/display/display.hpp`. `cmake --build build --target CAD_OpenGL` succeeded (2026-05-11).

## Mini retro

Centralizing “always on” preview in `RefreshStructurePreviewForRenderer` is clearer than leaving dead toggles in `Display`. If tuning returns, prefer app Settings or a dev-only overlay over resurrecting a large tool panel.
