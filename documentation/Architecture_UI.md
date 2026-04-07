# Architecture: UI — Grid-Based Panel Layout System

## Executive Summary

The UI module is a lightweight, custom-built 2D overlay system for rendering panels and handling button clicks. It uses an 80-column grid layout with anchor-based positioning, rounded-rectangle rendering via triangle-fan meshes, and a simple button dispatch system. The module consists of `UIRenderer` (GL rendering + layout resolution), `Panel` (constraint-based layout definition), `Button` (click callback binding), and `UIGrid` (screen-to-grid math). It is ~500 lines across 5 files plus 2 shader files.

**Verdict: Approve with changes**

**Top 3 strengths:**
1. Anchor-based layout system — panels are positioned relative to screen edges or other panels, with automatic gap handling. This enables responsive layouts that adapt to window resize.
2. Efficient mesh building — rounded rectangles use a center-vertex fan with configurable corner segments, rebuilt only on dirty flag.
3. Clean hit-testing — pixel-to-grid conversion with reverse-iteration for proper z-order respects visual layering.

**Top 3 concerns:**
1. No text rendering — panels are solid-colored rectangles with no labels, icons, or visual feedback.
2. No hover/active states — buttons have no visual indication of interactivity or press state.
3. UI definition is hardcoded in `Display::InitUI()` — the UI module itself has no coupling issues, but its consumer embeds layout directly.

---

## 1. Requirements & Motivation

### Functional Requirements
- Render 2D panel overlays on top of the 3D viewport.
- Position panels via anchor constraints (relative to screen edges or other panels).
- Support rounded-corner rectangles with configurable border radius.
- Handle click events on panels that have associated button callbacks.
- Provide hit-testing so that viewport interactions (orbit, pan) are blocked when the cursor is over UI.
- Respond to window resize by recalculating grid dimensions and panel positions.

### Non-Functional Requirements
- Minimal overdraw — only render visible panels.
- Rebuild mesh only when layout changes (dirty flag optimization).
- Sub-millisecond hit-test performance for real-time event routing.

### Constraints
- OpenGL 3.3 core profile — no immediate-mode rendering.
- Custom implementation — no ImGui or other UI library dependency.
- 2D screen-space rendering with orthographic projection.

---

## 2. Solution Description

### Components

| Component | Files | Responsibility |
|-----------|-------|----------------|
| **UIRenderer** | `UIRenderer.hpp/cpp` | Owns GL resources (VAO/VBO/IBO), shader, panel list, button list. Resolves anchors, builds rounded-rect meshes, renders with ortho projection, handles hit-testing and click dispatch. |
| **Panel** | `Panel.hpp` | Data struct defining a UI panel: anchor constraints (left/right/top/bottom), optional fixed width/height, color, border radius, visibility. Resolved position stored as `col/row/colSpan/rowSpan`. |
| **PanelAnchor** | `Panel.hpp` | Constraint: reference panel pointer (nullptr = screen edge) + edge enum (Left/Right/Top/Bottom). |
| **Button** | `Button.hpp` | Binding: panel ID string + `std::function<void()>` callback. |
| **UIGrid** | `UIGrid.hpp` | Screen-to-grid math: 80 columns, computed `cellSize` and `rows` from window dimensions. Converts cell units to pixels. |
| **Shaders** | `ui.vert/ui.frag` | Minimal 2D pipeline: `vec2` position + `vec4` color, projected by `uProjection` ortho matrix. |

### Layout System

The anchor system works as follows:

**Constraint model**: Each panel has up to 4 optional anchors (left, right, top, bottom) and optional fixed width/height. The resolver computes the panel's grid position from the constraints:

| Constraints Provided | Resolution |
|---------------------|------------|
| left + right | `col = left, colSpan = right - left` |
| left + width | `col = left, colSpan = width` |
| right + width | `col = right - width, colSpan = width` |
| top + bottom | `row = top, rowSpan = bottom - top` |
| top + height | `row = top, rowSpan = height` |
| bottom + height | `row = bottom - height, rowSpan = height` |

**Anchor resolution**: When an anchor references `nullptr` (screen edge):
- `Left/Top` → `GAP` (0.5 cells inward from screen edge)
- `Right` → `columns - GAP`
- `Bottom` → `rows - GAP`

When an anchor references another panel:
- `Left` → `panel.col - GAP` (left of the referenced panel, with gap)
- `Right` → `panel.col + panel.colSpan + GAP` (right of the referenced panel, with gap)
- `Top` → `panel.row - GAP`
- `Bottom` → `panel.row + panel.rowSpan + GAP`

**Resolution order**: Single linear pass over the `std::deque<Panel>`. Panels can only reference panels added before them (forward-only dependency).

### Data Flow

```
Window resize → UIRenderer::SetScreenSize(w, h)
├── grid.Update(w, h) → cellSize, rows
├── ResolveAnchors()  → panel col/row/colSpan/rowSpan
├── projection = ortho(0, w, h, 0, -1, 1)
└── dirty = true

UIRenderer::Render()
├── if dirty: BuildMesh()
│   ├── ResolveAnchors()
│   └── for each visible panel:
│       ├── if borderRadius > 0:
│       │   └── rounded rect: center vertex + 4 × (SEGMENTS+1) perimeter vertices
│       │       → triangle fan indices
│       └── else:
│           └── simple quad: 4 vertices, 2 triangles
│   └── Upload vertices + indices to VBO/IBO
│
├── glDisable(GL_DEPTH_TEST)
├── glEnable(GL_BLEND)
├── shader.Use(), shader.SetMat4("uProjection", projection)
└── glDrawElements(GL_TRIANGLES, indexCount)

Click event → UIRenderer::HandleClick(pixelX, pixelY)
├── pixelX / cellSize → cellX
├── pixelY / cellSize → cellY
└── for each button (reversed):
    └── if cellX/cellY inside panel bounds → onClick()
```

### Key Abstractions

- **Grid-based coordinates**: All layout computations use cell units (80 columns). Pixel conversion happens only at mesh-build time. This makes the layout resolution-independent.
- **Anchor constraints**: Declarative layout — panels describe *where* they want to be, not pixel positions. The resolver computes actual positions.
- **Panel deque + pointer stability**: `std::deque<Panel>` ensures that panel pointers in `PanelAnchor` remain valid as new panels are added.
- **Button-Panel binding**: Buttons reference panels by string `id`, not pointer. `HandleClick()` looks up the panel by ID to get bounds.

---

## 3. Design Principles

### Conforming

| Principle | Evidence |
|-----------|----------|
| **SRP** | `UIGrid` handles math, `Panel` holds layout data, `Button` holds click binding, `UIRenderer` handles GL rendering + layout resolution. |
| **OCP** | New panel types could be added (e.g., text panels, image panels) without modifying existing code — just extend the mesh builder. |
| **RAII** | `UIRenderer` cleans up GL resources in destructor, has proper move semantics. |

### Deliberately Relaxed

| Principle | Deviation | Justification |
|-----------|-----------|---------------|
| **SRP** | `UIRenderer` handles both layout resolution and GL rendering | At this scale, splitting into `UILayoutResolver` + `UIRenderer` would add a class for ~30 lines of logic. |
| **DIP** | `Button` uses `std::string` ID to reference panels (looked up at click time) | Pragmatic choice — avoids lifetime issues with panel pointers during deque growth. |

---

## 4. Alternatives Considered

| Decision | Current Approach | Alternative | Verdict |
|----------|-----------------|-------------|---------|
| UI framework | Custom grid-based renderer | Dear ImGui | ImGui is immediate-mode and wouldn't integrate cleanly with the retained-mode architecture. Custom is appropriate for a minimal panel-based UI. |
| Layout system | Anchor-based | Absolute pixel positioning | Anchor-based adapts to window resize automatically. **Current is better.** |
| Rounded rectangles | Triangle fan from center vertex | Shader-based SDF rounding | GPU-side SDF would be more flexible (variable border, shadow) but adds shader complexity. Triangle geometry is sufficient. |
| Panel storage | `std::deque` | `std::vector<unique_ptr>` | Deque gives pointer stability for anchor references. Same rationale as Scene module. **Correct.** |
| Button dispatch | Reverse iteration over buttons | Event bubbling / z-order tree | Reverse iteration matches visual z-order (last-added = top). Simple and correct for a flat panel hierarchy. |

---

## 5. Technology & Dependencies

| Library | Role in UI |
|---------|-----------|
| GLAD/OpenGL | VAO/VBO/IBO, shader program, blending, ortho projection |
| GLM | `glm::vec2/vec4` for vertices, `glm::ortho` for projection |
| SDL3 | Window size queries (`SDL_GetWindowSize`) |

No new dependencies. The UI module is entirely self-contained.

### Shader Program

| Shader | Inputs | Uniforms | Purpose |
|--------|--------|----------|---------|
| `ui.vert` | `vec2 aPosition`, `vec4 aColor` | `mat4 uProjection` | Transform 2D screen positions |
| `ui.frag` | `vec4 fragColor` (from vertex) | — | Pass-through color with alpha |

---

## 6. Tradeoffs

| Decision | Chosen Approach | What's Sacrificed | Rationale |
|----------|----------------|-------------------|-----------|
| No text rendering | Simplicity | User information (labels, values) | Text rendering requires font atlas, glyph layout — significant complexity. Panels are identifiable by position/color for now. |
| No hover/active states | Simplicity | Interactive feedback | Would require per-frame mouse tracking and mesh rebuild on hover. Can be added later. |
| Fixed 80-column grid | Predictable layout | Fine-grained control at high resolutions | 80 columns provides sufficient granularity. At 1280px, each cell is 16px — adequate. |
| 8 segments per corner | Visual quality | Performance (negligible) | 8 segments per quarter-circle produces smooth-looking corners. Could be reduced for performance. |
| Rebuild all panels on any change | Simplicity | Efficiency for many panels | With few panels, full rebuild is < 1ms. Per-panel dirty tracking would be premature. |

---

## 7. Best Practices Compliance

### Conforming
- **RAII**: GL resources cleaned in `Shutdown()` called from destructor. Move semantics handle ownership transfer.
- **Dirty flag optimization**: Mesh is rebuilt only when `dirty = true` (screen resize, panel add). No per-frame rebuild.
- **Correct blending**: `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA` with depth test disabled for 2D overlay.
- **Rounded rectangle geometry**: Center-vertex fan is the correct approach for rounded rects in a triangle-based pipeline.
- **Pointer stability**: `std::deque` for panels ensures anchor pointers remain valid.

### Non-Conforming

| Issue | Severity | Details | Remediation |
|-------|----------|---------|-------------|
| No text rendering | Major (UX) | Panels are solid-colored rectangles with no visual labels | Add a text rendering system (bitmap font atlas or SDF font rendering) |
| No hover/active states | Minor (UX) | Buttons have no visual feedback on hover or press | Track mouse position per-frame, change panel color on hover, flash on click |
| Button lookup by string ID | Minor | `HandleClick()` iterates buttons and calls `GetPanel(id)` for each — O(buttons × panels) per click | Cache resolved panel pointers, or store `Panel*` directly (safe since deque is stable) |
| `AddButton` creates a duplicate panel | Minor | `AddButton(panel, onClick)` pushes the panel into the deque AND creates a button — the same panel is both a visual element and a click target, but the button references the original panel's `id`, not the pushed copy | Clarify API: either `AddButton` should reference an existing panel, or it should be the sole way to create clickable panels |
| No panel removal API | Minor | Panels can be added but not removed or hidden dynamically (beyond the `visible` flag) | Add `RemovePanel(id)` if dynamic UI is needed |
| `UIGrid::GAP` is `static constexpr` | Minor | Gap cannot be adjusted per-panel or at runtime | Acceptable for uniform spacing; could be made per-panel if needed |

---

## 8. Risks & Future-Proofing

### Risks

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| No text = unclear UI | High (UX) — users can't identify panels or understand what buttons do | High | Implement bitmap font or SDF text rendering |
| Panel hierarchy depth | Low — anchor resolution is a single linear pass | Low | If cyclic anchors are defined, the resolver won't detect them (but the data model prevents cycles since panels reference only earlier panels) |
| Many panels | Low — mesh rebuild is O(panels × segments) | Low | Only a few panels in current UI |

### Future Considerations
- **Text rendering**: The highest-impact addition. Options: bitmap font atlas (simple), stb_truetype (moderate), SDF fonts (best quality).
- **Panel hierarchy / nesting**: Current anchor system is flat. True nesting would require recursive layout resolution or a flexbox-like model.
- **Scrollable lists**: For file browsers or analysis result lists, panels would need scroll containers with clipping.
- **Theming**: Centralize colors, spacing, and typography in a theme struct.
- **Animation**: Panel slide-in/fade transitions would require per-frame interpolation and dirty-flag updates.
- **DPI awareness**: Current grid assumes 1:1 pixel mapping. Retina/HiDPI displays may need scale factor handling.

---

## 9. Recommendations

### Must-Have
1. **Add text rendering** — at minimum, bitmap font atlas for panel labels and button text. Panels without labels are not usable.

### Should-Have
2. **Hover/active visual states** — change panel color on mouse hover, flash on click. Requires per-frame mouse position tracking.
3. **Clarify AddButton API** — either buttons reference existing panels, or `AddButton` is the sole creation path for interactive panels. Current dual-push creates ambiguity.

### Nice-to-Have
4. **Theming system** — extract colors and spacing into a centralized theme struct.
5. **DPI scaling** — query display scale factor and adjust `UIGrid::cellSize` accordingly.
6. **Panel removal** — `RemovePanel(id)` for dynamic UI updates (e.g., closing a properties panel).
7. **Tooltip support** — hovering over a button shows a tooltip with description text.
