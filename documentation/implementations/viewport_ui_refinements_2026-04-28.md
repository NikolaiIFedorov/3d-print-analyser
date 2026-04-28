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

---

## Follow-up (remove grid LOD; rely on transparency)

### Problem

Zoom / pixel-gap grid LOD felt worse than a fixed dense grid.

### Approach

- Removed `gridWorldSpacing` / `PickGridWorldSpacing` and per-frame `RegenerateGrid` from `SetCamera`; grid always uses world spacing `1.0`.
- `basic.frag`: lower alpha range for the grid pass (`aLo` / `aHi` with principal-snap mix) so the floor stays readable but dense lines stack visually lighter.

### Files

- `ViewportRenderer.{hpp,cpp}`, `basic.frag`, this log

---

## Follow-up (depth priority + bottom inset)

### Problem

- Z-fighting between mesh faces and the floor grid; user wanted geometry to win over the grid.
- Z-fighting between axes and grid at the origin plane; user wanted axes preferred.
- Bottom gap between Settings/Toolbar and window still too tall.

### Approach

- Grid pass: `GL_POLYGON_OFFSET_LINE` with positive `glPolygonOffset` (negative when reverse-Z) so grid depth is pushed slightly **farther** than coplanar fills/lines.
- Axes pass: `GL_POLYGON_OFFSET_LINE` with opposite sign so axis lines sit slightly **closer** than the biased grid (still after scene depth test; stencil unchanged).
- `UIGrid::SCREEN_BOTTOM_INSET` reduced from `0.85f` to `0.35f` cells.

### Files

- `ViewportRenderer.cpp`, `UIGrid.hpp`, this log

---

## Follow-up (explicit depth stack: axes > grid > scene)

### Intended order

Front to back: **axes** over **grid** over **scene** triangles/edges (reduce z-fighting at the floor plane).

### Approach

- Grid pass: `GL_POLYGON_OFFSET_LINE` with **negative** factor (standard depth) so stored grid depth is slightly **nearer** than coplanar mesh; scene draws afterward and loses where the user wants the grid to read on top.
- Axes pass: **more negative** line offset than grid so axes beat the grid at shared lines; reverse-Z uses negated constants.
- Named constants `kGridLinePolygonOffset` / `kAxisLinePolygonOffset` in `ViewportRenderer.cpp`.

### Files

- `ViewportRenderer.cpp`, `display.cpp` (comment), this log

---

## Follow-up (clip-space Z bias vs z-fighting)

### Problem

`GL_POLYGON_OFFSET_LINE` alone still allowed shimmer between grid, axes, and mesh (slope-dependent and small ULP separation).

### Approach

- `RenderingExperiments`: `kClipZBiasSceneMeshW`, `kClipZBiasGridW`, `kClipZBiasAxesW` plus `ClipZBiasSceneMeshW()` / `ClipZBiasGridW()` / `ClipZBiasAxesW()` (sign flip when `kReverseZDepth`).
- `basic.vert`: `gl_Position.z += uClipZBiasW * pos.w` — scene patches use **positive** bias (farther); grid/axes use **more negative** (nearer), preserving axes > grid > scene.
- `line.vert`: same `uClipZBiasW` for wireframe with scene bias so edges share the mesh depth layer.
- Removed grid/axes `GL_POLYGON_OFFSET_LINE` passes; pick highlight / pick lines set `uClipZBiasW` to 0.

### Files

- `include/RenderingExperiments.hpp`, `basic.vert`, `line.vert`, `OpenGLRenderer.cpp`, `ViewportRenderer.cpp`, `display.cpp`, this log

---

## Follow-up (grid bleed on solids + LOD readability)

### Problem

- Grid and axes could still read through opaque fills when depth alone was ambiguous.
- Coarse grid LOD (wider world spacing) made the floor hard to see if alpha stayed low.

### Approach

- **Draw order**: `display.cpp` draws opaque patches (stencil write `1`), wireframe, pick-highlight lines, then **grid** then **axes**; stencil test `EQUAL 0` on the grid pass so fragments over opaque patch pixels are skipped.
- **LOD**: Restore world-spacing powers-of-two with hysteresis in `ViewportRenderer::SetCamera` (`DesiredGridLodSpacing`, `ApplyGridLodHysteresis`); `RegenerateGrid` when spacing changes.
- **`uGridLodStep`**: Grid pass sets this to current `gridWorldSpacing`; `basic.frag` adds a small alpha boost when spacing > 1 so sparse lines stay visible without globally raising opacity.

### Files

- `display.cpp`, `ViewportRenderer.{hpp,cpp}`, `basic.frag`, `OpenGLRenderer.cpp`, this log

### Outcome

Build clean; user can tune hysteresis / `kMinPx` / LOD max in `ViewportRenderer.cpp` if stepping feels harsh.

---

## Follow-up (grid vanishes when zoomed into a face)

### Problem

Stencil mask `EQUAL 0` hides the grid on every pixel where opaque triangles ran. A large face
coplanar with the reference plane fills the viewport with stencil=1, so the grid disappears
entirely when the user zooms in close.

### Approach

- **Two-pass grid**: keep the stencil==0 pass; when `|viewDirWorld.z| > 0.62`, add a second draw
  with `GL_EQUAL 1` so coplanar horizontal inspection recovers the floor grid (depth + clip bias
  still suppress the grid through vertical walls at grazing views).
- **`basic.frag`**: slightly wider grazing smoothstep and higher `aLo` so edge-on views stay faintly
  visible.

### Files

- `ViewportRenderer.{hpp,cpp}`, `basic.frag`, this log

---

## Follow-up (grid LOD too coarse)

### Problem

Spacing doubled to 2 world units as soon as `kMinPx * wpp` passed 1 (smallest power-of-two ≥ need),
so moderate zoom looked too sparse; refine hysteresis at 0.68 also kept coarse steps when zooming in.

### Approach

- Lower `kMinPx` and choose spacing with `while (s * 2 <= need * kLodStepSlack)` so 1× persists until
  `need` clearly exceeds the next band.
- Refine hysteresis 0.68 → 0.78.

### Files

- `ViewportRenderer.cpp`, this log

---

## Follow-up (grid LOD: ≥ 1 px between parallel lines)

### Problem

Heuristic `kMinPx` / slack still felt too coarse; user asked for a clear rule.

### Approach

- With `wpp` = world units per pixel (same ortho span / min viewport side), spacing `s` implies
  ~`s/wpp` pixels between parallel lines. Enforce **≥ 1 px**: use smallest power-of-two
  `s ∈ {1,…,32}` with `s >= wpp` (tiny epsilon on float compare). Max 32 caps extreme zoom-out.
- Refine hysteresis eased to 0.92 so zoom-in tracks the finer desired spacing quickly.

### Files

- `ViewportRenderer.{hpp,cpp}`, this log

---

## Follow-up (grid LOD not updating on zoom)

### Problem

- `Camera::widthWindow` / `heightWindow` were never set (only `aspectRatio` in ctor / resize), so
  grid LOD used stale or zero dimensions with `ViewportRenderer::SetCamera`.
- With the ≥1 px rule, smallest spacing in `{1,2,4,…}` with `s >= wpp` is always **1** whenever
  `wpp < 1` (typical CAD zoom), so LOD never moved; `Generate()` also clamped with `max(1, spacing)`.

### Approach

- Set `widthWindow` / `heightWindow` in `Camera` ctor and `SetAspectRatio(aspect, w, h)`; call the
  new overload from `Display::SetAspectRatio`; default members for safety.
- LOD spacing: dyadic ladder from `1/256` to `32` world units, `minWorldSpacing = max(1px·wpp,
  densityFloor)` where `densityFloor = 2*GRID_EXTENT / (4·max(w,h))` caps mesh size.
- `Generate()`: allow sub-unit spacing (`max(1/8192, gridWorldSpacing)`); hysteresis uses ~6%
  relative band; regenerate when relative delta exceeds a small epsilon.

### Files

- `Camera.{hpp,cpp}`, `display.cpp`, `ViewportRenderer.{hpp,cpp}`, this log

---

## Follow-up (grid LOD vs orbit / tilt)

### Problem

LOD used ortho scale and window size only. Orbit changes tilt without changing `orthoSize`, so
parallel XY grid lines could pack tighter on screen at shallow angles while LOD stayed on a
finer world step.

### Approach

- Scale effective `wpp` by `1 / max(kFloor, |viewDir·ẑ|)` (same `viewDirWorld.z` already used for grid
  shading): foreshortening makes the same world spacing “denser” in pixels — same ≥1 px rule as zoom.
- Small floor `kForeshortenFloor` avoids extreme coarsening at grazing views.

### Files

- `ViewportRenderer.cpp`, this log

### Follow-up (stronger orbit / tilt LOD)

- `kForeshortenFloor` 0.07 → 0.035; apply `wpp = wppLinear / pow(foreshort, 1.5)` instead of linear
  `1/foreshort` so shallow tilts step LOD coarser sooner.

### Follow-up (orbit LOD strength dial-back)

- Floor `0.055`, exponent `1.28` — between original linear / 0.07 floor and the too-strong 0.035 / 1.5.

---

## Follow-up (grid opacity: LOD only)

### Problem

View-based grazing alpha in `basic.frag` plus LOD foreshortening felt redundant.

### Approach

- Grid pass: fixed base alpha (~0.46) plus existing `uGridLodStep` boost for coarse spacing; removed
  `uViewDirWorld` / `uPrincipalSnap` from `basic.frag` and CPU setters. Dropped `principalSnapForGrid`
  from `ViewportRenderer` (wireframe principal-axis nudge in `SceneRenderer` unchanged).

### Files

- `basic.frag`, `OpenGLRenderer.cpp`, `ViewportRenderer.{hpp,cpp}`, this log

---

## Follow-up (principal-axis snap: easier in, harder out)

### Problem

Orbit snap used one cone (~3°): leaving a snapped canonical view re-snapped on small motion.

### Approach

- Hysteresis: **enter** still `cos(3°)` via `TryPrincipalSnapQuat`; when latched, keep
  `latchedPrincipalOrientation` until raw orbit forward is **> ~5.25°** off that axis (was 8.5° —
  too sticky vs enter-only 3°).
- Latch cleared on `Roll`, `FrameBounds`, `ResetHomeView`.

### Follow-up (snap-out too sticky still)

- Reduced snap-out threshold to `~4.2°` while keeping enter at `3°`, preserving hysteresis but making
  leaving a snapped orientation require less drag.

### Files

- `Camera.{hpp,cpp}`, this log
