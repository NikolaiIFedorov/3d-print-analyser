# Viewport grid/axes + UI bottom inset (2026-04-28)

## Problem

- Settings and toolbar panels were anchored to the screen bottom with only `GAP/2` (0.25 cell) inset, so they sat flush on the window frame.
- At shallow tilts or zoom, dense world XY grid lines stacked in screen space and read as an opaque grey sheet; little structure visible “through” the grid.
- Axis segments used a heuristic extent (`max(grid diameter, orthoSize*(aspect+1)*2)`) that did not follow the actual ortho frustum along each principal direction after rotation, so axes could clip short of the viewport edges.

## Approach

- `UIGrid::SCREEN_BOTTOM_INSET` (1 cell): subtract from screen bottom in `ResolveAnchors` and `ComputeMinGridSize` so bottom-anchored root panels leave a consistent gap.
- Grid-only pass: `basic.frag` scales unlit line RGB by `smoothstep` on `|dot(viewDir, Z)|` when `uGridPlaneFade` is on; `ViewportRenderer` sets `uViewDirWorld` from camera forward. Axes pass clears the fade flag. `OpenGLRenderer` sets fade off for lit meshes.
- `OrthoClipAxisWorldHalfExtent`: ray–ortho-slab intersection from world origin along ±X/±Y/±Z in view space using current `near`/`far` and ortho half-width/height; take max exit `h`, margin, and `max` with `2*GRID_EXTENT`.

## Files

- `src/display/rendering/UIRenderer/UIGrid.hpp`, `UIRenderer.cpp`
- `src/display/rendering/OpenGL/shaders/basic.frag`
- `src/display/rendering/OpenGL/OpenGLRenderer.cpp`
- `src/display/display.cpp`
- `src/display/rendering/ViewportRenderer/ViewportRenderer.hpp`, `ViewportRenderer.cpp`

## Outcome

`cmake --build build` succeeds. Settings/toolbar gain ~1 cell (~40px at 1× scale) bottom breathing room; grid fades when the view direction is nearly parallel to the XY plane; axis half-length follows the ortho slab from the world origin so lines reach the viewport edges before clip (still at least `2 * GRID_EXTENT`).

## Ship

Committed as `c5640cf` on `imgui-refactor`.

## Mini retro

Centralizing screen-edge insets in `UIGrid` keeps `ResolveAnchors` / min-grid simulation aligned; axis extent from slab math removes magic ortho multipliers but still depends on previous-frame `near`/`far` (one-frame lag acceptable).
