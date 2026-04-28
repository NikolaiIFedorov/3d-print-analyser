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

Shipped on `imgui-refactor` with the viewport/UI/grid/axes change set (see `git log` for hash).

## Mini retro

Centralizing screen-edge insets in `UIGrid` keeps `ResolveAnchors` / min-grid simulation aligned; axis extent from slab math removes magic ortho multipliers but still depends on previous-frame `near`/`far` (one-frame lag acceptable).

---

## Follow-up (same theme)

### Problem

- Grid grazing fade used RGB multiply and could read as “too gone”; user wanted partial transparency with a floor.
- Zoomed-out views still drew unit-spaced world lines (heavy LOD).
- Bottom gap still invisible: StatusStrip post-pass moved `Settings`/`Toolbar` down by `dy` but left `rowSpan` unchanged, so panels extended past the anchored bottom and painted over the `SCREEN_BOTTOM_INSET` band.

### Approach

- `basic.frag`: grid pass outputs premultiplied-style `vec4(c, a)` with `a` in `[0.26, 0.92]` from view–plane alignment; `ViewportRenderer::Render` enables blending and disables depth writes for the grid pass only.
- `PickGridWorldSpacing(ortho, aspect)`: powers of two in `[1, 256]` from ortho half-extent; `SetCamera` bumps `gridWorldSpacing` and `RegenerateGrid` when it changes.
- After strip shift: `rowSpan -= dy` for Settings and Toolbar; `SCREEN_BOTTOM_INSET` raised to `1.75f` cells.

### Files

- `UIGrid.hpp`, `UIRenderer.cpp`, `basic.frag`, `ViewportRenderer.{hpp,cpp}`

---

## Follow-up (bottom gap tuning, depth, LOD, principal views)

### Problem

- Bottom gap felt too large after strip `rowSpan` fix.
- Power-of-two grid LOD was too aggressive; user preferred a minimum **screen-pixel** gap between lines.
- Scene appeared to draw on top of grid/axes: grid pass used `glDepthMask(GL_FALSE)`; axes used `glDisable(GL_DEPTH_TEST)`.
- Wireframe back edges popped in front of faces on principal-axis snaps (depth precision + small line nudge).
- Request: slightly stronger faint grid when snapped to a canonical orientation.

### Approach

- `SCREEN_BOTTOM_INSET` → `0.85f` cells.
- `PickGridWorldSpacing`: `ceil(kMinPixelGap * max(worldPerPxX, worldPerPxY))` with `kMinPixelGap = 2.5`, cap 512.
- Grid pass: `glDepthMask(GL_TRUE)` again (keep blend for grazing alpha).
- `RenderAxes`: depth test **on**, `glDepthMask(GL_FALSE)`, same depth func as scene; stencil still masks solid pixels from patch pass.
- `Camera::IsPrincipalAxisView` (3° cone, same as snap): `uPrincipalSnap` raises grazing alpha floor in `basic.frag`; `SceneRenderer::SetCamera` sets `SetWireframeDepthNudgeScale(2.25f)` on principal views for wireframe Z nudge and polygon-offset scale.

### Files

- `UIGrid.hpp`, `Camera.{hpp,cpp}`, `ViewportRenderer.{hpp,cpp}`, `basic.frag`, `OpenGLRenderer.{hpp,cpp}`, `SceneRenderer.cpp`
