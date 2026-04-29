# Architecture: UI — Hierarchical Panel Layout System

## Executive Summary

The UI module is a retained-mode 2D overlay system for rendering hierarchical panels with rich text content and interactive controls. It combines an OpenGL mesh layer (rounded-rectangle backgrounds, rendered via a center-vertex triangle-fan) with an ImGui overlay layer (text, icons, drag sliders, segmented pill selectors). Layout is computed in a three-level hierarchy — `RootPanel → Section → Paragraph` — anchored to screen edges or sibling panels using a fixed 40 px-per-cell grid with DPI scaling. The module spans ~10 files plus 2 shader files: `UIRenderer`, `Panel.hpp`, `TextRenderer`, `UIGrid`, `PanelGrid`, `UIStyle`, `Icons`, and the `ui.vert`/`ui.frag` shaders.

**Verdict: Approve**

**Top 3 strengths:**
1. Dual-layer rendering — GL mesh handles backgrounds and rounded rects; ImGui overlay handles text, icons, and interactive widgets without an additional text rendering pipeline.
2. Hierarchical panel model (`RootPanel → Section → Paragraph`) with a CSS-style box model (margin/padding/lineGap) scales naturally to multi-section information panels.
3. `SectionLine`-based content model enables per-line interactivity (click, icon, drag slider, pill selector) without introducing separate button objects.

**Top 3 concerns:**
1. `UIRenderer::Render()` mixes GL mesh building with ImGui draw-list calls — two rendering paths interleaved in one method make the render loop harder to follow.
2. `SetSectionVisible` / `SetSectionValue` retain the old string-based lookup API alongside direct `Paragraph*` access in `Display`, creating two competing update patterns.
3. `Section::collapsed` state is stored on the element but the renderer controls expand/collapse — the UI element is not fully self-contained.

---

## 1. Requirements & Motivation

### Functional Requirements
- Render 2D panel overlays on top of the 3D viewport.
- Position panels via anchor constraints (relative to screen edges or other panels).
- Support rounded-corner rectangles with configurable border radius.
- Render text labels, section headers, and per-line icons inside panels.
- Support interactive per-line elements: clickable text rows, DragFloat sliders, segmented pill selectors.
- Provide hit-testing so that viewport interactions (orbit, pan) are blocked when the cursor is over UI.
- Respond to window resize by recalculating grid dimensions and panel positions.
- Enforce a minimum window size derived from the panel layout.
- Support dark and light themes via the `Color` module.

### Non-Functional Requirements
- Minimal overdraw — only render visible panels.
- Rebuild GL mesh only when layout changes (dirty flag optimization).
- Sub-millisecond hit-test performance for real-time event routing.
- Scale correctly on HiDPI / Retina displays via a `displayScale` factor.

### Constraints
- OpenGL 3.3 core profile for the background mesh.
- ImGui (SDL3 + OpenGL3 backend) for the text and interactive widget overlay.
- 2D screen-space rendering with orthographic projection.

---

## 2. Solution Description

### Components

| Component | Files | Responsibility |
|-----------|-------|----------------|
| **UIRenderer** | `UIRenderer.hpp/cpp` | Owns GL resources (VAO/VBO/IBO), panel deque, `TextRenderer`, and ImGui font handles. Resolves anchors, builds the background mesh, renders it with ortho projection, draws the ImGui overlay, and handles hit-testing. |
| **RootPanel** | `Panel.hpp` | Top-level anchor-positioned container. Owns `Section` and `Paragraph` children via `std::vector<ChildElement>`. Carries optional `Header`, anchor constraints, optional fixed width/height, background color depth. |
| **Section** | `Panel.hpp` | Mid-level labeled container. No background. Owns `Paragraph` children. Optional `Header`, `collapsed` flag. Splitters between children are drawn by the parent layout algorithm. |
| **Paragraph** | `Panel.hpp` | Leaf element. Owns `std::vector<SectionLine>`. Each line can carry icon, click callback, DragFloat widget, or `Select` pill. |
| **UIElement** | `Panel.hpp` | Base struct for `RootPanel`, `Section`, `Paragraph`. Holds resolved grid position (col/row/colSpan/rowSpan), border radius, margin, padding, lineGap, `PanelGrid localGrid`, and `ContentBox box`. |
| **Header** | `Panel.hpp` | Display label for `RootPanel` or `Section`. Backed by a `Paragraph` for uniform rendering. Constructor uppercases text and sets `bold = true`. |
| **SectionLine** | `Panel.hpp` | Per-line content record: `prefix`+`text` (colored prefix + default text), `iconDraw`, `onClick`, `imguiContent`, `Select`, `bold`, `fontScale`, `textDepth`, `selected`. |
| **Select** | `Panel.hpp` | Segmented pill selector widget. Owns an ordered list of `SelectOption` (label + icon). Rendered as variable-width zones with an accent pill behind the active option. |
| **PanelAnchor** | `Panel.hpp` | Constraint: `const RootPanel*` reference (nullptr = screen edge) + `Edge` enum (Left/Right/Top/Bottom). |
| **PanelGrid** | `PanelGrid.hpp` | Local coordinate system inside a panel. Cell size = `globalCell × LOCAL_CELL_RATIO (0.5)`. Converts panel-local cell coords to absolute pixel positions. |
| **UIGrid** | `UIGrid.hpp` | Global grid math. Fixed `CELL_SIZE = 40 px`, scaled by `displayScale`. Tracks `columns`/`rows` (window extent) and `minColumns`/`minRows` (layout-derived minimum). |
| **TextRenderer** | `TextRenderer.hpp/cpp` | FreeType-based bitmap glyph atlas. Renders text via GL quads. Provides `MeasureWidth`, `GetLineHeight`, `GetMaxBearingY`, and `MeasureBounds` for layout measurement. |
| **TextMetrics** | `UIRenderer.hpp` | Precomputed per-frame text sizing: `localCell`, `textScale`, `textHeightCells`. Derived from `UIGrid` + `TextRenderer`. |
| **Icons** | `Icons.hpp` | Programmatic icon drawing functions. Each factory returns a `DrawFn = std::function<void(ImDrawList*, float x, float midY, float s)>`. Icons include `ImportFile`, `Placeholder`, `Overhang`, `SharpCorner`, and others. Stroke weight scales with `s × STROKE_RATIO`. |
| **UIStyle** | `UIStyle.hpp` | Shared style constants and `PushInputStyle`/`PopInputStyle` helpers for DragFloat and framed widgets. `DrawInputHoverTint` draws accent tints for hover/active states. |
| **PixelBounds** | `Panel.hpp` | Pixel-space AABB built incrementally via `expand()`. Used by `TextRenderer::MeasureBounds` and layout to measure actual rendered extents. |
| **Shaders** | `ui.vert`/`ui.frag` | Minimal 2D pipeline: `vec2 aPosition` + `vec4 aColor`, projected by `uProjection` ortho matrix. Pass-through color with alpha. |

### Panel Hierarchy

```
RootPanel  (anchor-positioned, optional background, optional header)
├── Section  (labeled group, no background, optional header, collapsible)
│   ├── Paragraph  (leaf, SectionLine content)
│   └── Paragraph  ...
├── Paragraph  (direct child, no section grouping)
└── ...
```

Each level carries independent margin/padding/lineGap derived from `UIGrid::GAP × INSET_RATIO`:
- `RootPanel` (layer 0): margin + padding
- `Section` (layer 1): no margin (0), padding
- `Paragraph` (layer 2): margin, no padding (0)

### Layout System

The anchor system works as follows:

**Constraint model**: Each `RootPanel` has up to 4 optional anchors (left, right, top, bottom) and optional fixed width/height. The resolver computes the panel's grid position from the constraints:

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

When an anchor references another `RootPanel`:
- `Left` → `panel.col - GAP`
- `Right` → `panel.col + panel.colSpan + GAP`
- `Top` → `panel.row - GAP`
- `Bottom` → `panel.row + panel.rowSpan + GAP`

**Resolution order**: Single linear pass over `std::deque<RootPanel>`. Panels can only reference panels added before them (forward-only dependency).

**Content sizing**: After anchor resolution, a bottom-up content-height pass computes each `Paragraph`'s `ContentBox` from its `SectionLine` count and `TextMetrics`, then propagates upward through `Section` and `RootPanel` to determine `rowSpan`.

### Rendering Architecture

The renderer uses two independent layers drawn sequentially each frame:

**Layer 1 — GL background mesh** (`BuildMesh()` + `glDrawElements`):
- One rounded-rectangle per visible `RootPanel` and `Section` with a background.
- Splitter lines between `Section` children rendered as thin rectangles.
- Vertices: `UIVertex { vec2 position; vec4 color }`.
- Rebuilt only when `dirty = true` (resize, panel add, visibility change).

**Layer 2 — ImGui overlay** (`Render()` ImGui section):
- One `ImGui::SetNextWindowPos/Size` + transparent `ImGui::Begin/End` window per `Paragraph`.
- Text rendered via `ImGui::Text` / `ImGui::TextColored` using the appropriate font slot (`bodyImFont`, `heavyImFont`).
- Icons drawn via `ImDrawList` calls inside each `SectionLine`.
- Interactive widgets: `ImGui::DragFloat` for `imguiContent` rows; custom pill rendering for `Select` rows.
- Hover/active tints drawn by `UIStyle::DrawInputHoverTint`.

### Data Flow

```
Window resize → UIRenderer::SetScreenSize(w, h)
├── grid.Update(w, h, displayScale) → cellSizeX/Y, columns, rows
├── ResolveAnchors()  → RootPanel col/row/colSpan/rowSpan
├── ComputeMinGridSize() → grid.minColumns/minRows
├── projection = ortho(0, w, h, 0, -1, 1)
└── dirty = true

UIRenderer::Render()
├── if dirty: BuildMesh()
│   ├── ResolveAnchors()
│   ├── ComputeTextMetrics() → TextMetrics tm
│   └── for each visible RootPanel / Section with background:
│       ├── EmitRoundedRect(...)  → vertices + indices
│       └── (splitter lines between Section children)
│   └── Upload vertices + indices to VBO/IBO
│
├── glDisable(GL_DEPTH_TEST), glEnable(GL_BLEND)
├── shader.Use(), SetMat4("uProjection", projection)
├── glDrawElements(GL_TRIANGLES, indexCount)       ← GL mesh pass
│
└── for each visible Paragraph (all panels):       ← ImGui overlay pass
    ├── ImGui::SetNextWindowPos/Size
    ├── ImGui::Begin (transparent, no-input window)
    └── for each SectionLine in values:
        ├── iconDraw(dl, x, midY, s)    if present
        ├── ImGui::Text / TextColored   for prefix + text
        ├── SectionLine::onClick        if present (underline + hand cursor)
        ├── imguiContent(w, h, offset)  if present (DragFloat etc.)
        └── Select pill zones           if select.has_value()

Hit-test → UIRenderer::HitTest(pixelX, pixelY) → bool
├── for each visible RootPanel (reversed):
└── returns true if (pixelX, pixelY) is inside any panel bounds
    (click dispatch handled by ImGui via its own input routing)
```

### Key Abstractions

- **Grid-based coordinates**: All layout computations use global cell units. Pixel conversion happens at mesh-build time via `grid.ToPixelsX/Y()` and inside `PanelGrid`.
- **Two-layer rendering**: The GL mesh layer and ImGui overlay layer are independent and complementary — GL handles backgrounds, ImGui handles text and input.
- **Box model**: `UIElement` has explicit margin/padding/lineGap following CSS semantics. Space between siblings = `margin1 + margin2`.
- **`PanelGrid` local grid**: Each `RootPanel` carries a `PanelGrid` with half-size cells, enabling finer glyph positioning inside panels without polluting the global grid resolution.
- **`SectionLine` as universal content unit**: A single struct covers all per-line rendering modes (plain text, icon+text, clickable text, drag slider, pill selector). No separate button or widget type needed.
- **`PixelBounds`**: Incremental AABB for measuring actual rendered extents. Used by `TextRenderer::MeasureBounds` to derive content height during layout.
- **Panel deque + pointer stability**: `std::deque<RootPanel>` ensures that `RootPanel*` pointers in `PanelAnchor` remain valid as new panels are added.

---

## 3. Design Principles

### Conforming

| Principle | Evidence |
|-----------|----------|
| **SRP** | `UIGrid` handles grid math, `PanelGrid` handles local mesh coords, `TextRenderer` handles glyph rendering, `Icons` handles icon drawing, `UIStyle` handles input widget theming, `UIRenderer` orchestrates layout + GL + ImGui. |
| **OCP** | New line content modes can be added to `SectionLine` without modifying existing rendering code. New icon types are added to `Icons` without touching the renderer. |
| **RAII** | `UIRenderer` and `TextRenderer` clean up GL resources in their destructors; move semantics handle ownership transfer. |
| **DPI awareness** | `UIGrid::Update` accepts `displayScale`; all pixel computations multiply through it. |

### Deliberately Relaxed

| Principle | Deviation | Justification |
|-----------|-----------|---------------|
| **SRP** | `UIRenderer::Render()` drives both the GL mesh pass and the ImGui overlay pass | The two passes must execute in order within the same frame; separating them would require a shared frame-state object for minimal benefit. |
| **String-based API** | `SetSectionVisible` / `SetSectionValue` still exist alongside direct `Paragraph*` access | Legacy API retained for the few callsites that haven't been migrated. Should be removed when all callers use direct pointer access. |

---

## 4. Alternatives Considered

| Decision | Current Approach | Alternative | Verdict |
|----------|-----------------|-------------|---------|
| Text rendering | ImGui overlay (SDL3+OGL3 backend) | Custom FreeType bitmap atlas only | `TextRenderer` exists for GL-layer measurement (glyph metrics); ImGui handles interactive text. Using ImGui avoids re-implementing font shaping and input handling. |
| UI framework | Custom panel system + ImGui overlay | Pure ImGui (immediate mode) | Pure ImGui would require restructuring the retained-mode architecture. Hybrid keeps retained panel layout while leveraging ImGui's input handling and font system. |
| Layout system | Anchor-based | Absolute pixel positioning | Anchor-based adapts to window resize automatically. **Current is better.** |
| Rounded rectangles | Triangle fan from center vertex | Shader-based SDF rounding | SDF rounding adds shader complexity. Triangle geometry is sufficient at the current panel count. |
| Panel storage | `std::deque` | `std::vector<unique_ptr>` | Deque gives pointer stability for anchor references. **Correct.** |
| Interactivity | `SectionLine::onClick` + ImGui input routing | Separate `Button` class with reverse-iteration hit-test | ImGui handles hover detection and cursor changes natively; embedding callbacks in `SectionLine` eliminates a separate button registry. |
| Panel minimum size | `ComputeMinGridSize()` → SDL minimum window size | Unconstrained window | Prevents panels from being clipped when the window is too small. |

---

## 5. Technology & Dependencies

| Library | Role in UI |
|---------|-----------|
| GLAD/OpenGL | VAO/VBO/IBO, shader program, blending, ortho projection (background mesh) |
| ImGui (SDL3+OGL3) | Text rendering, interactive widgets (DragFloat, custom pill), icon draw calls via `ImDrawList` |
| GLM | `glm::vec2/vec4` for vertices, `glm::ortho` for projection |
| SDL3 | Window size queries, display scale factor |
| FreeType (via TextRenderer) | Glyph metrics and atlas build for layout measurement |

### Font Slots

`UIRenderer` holds three ImGui font handles:

| Slot | Member | Usage |
|------|--------|-------|
| `pixelImFont` | Pixel-perfect at small sizes | Legacy / fallback |
| `bodyImFont` | Regular weight | `SectionLine` body text (`textDepth ≤ 2`) |
| `heavyImFont` | Bold/header weight | `SectionLine` with `bold = true`; panel and section headers |

### Shader Program

| Shader | Inputs | Uniforms | Purpose |
|--------|--------|----------|---------|
| `ui.vert` | `vec2 aPosition`, `vec4 aColor` | `mat4 uProjection` | Transform 2D screen positions |
| `ui.frag` | `vec4 fragColor` (from vertex) | — | Pass-through color with alpha |

---

## 6. Tradeoffs

| Decision | Chosen Approach | What's Sacrificed | Rationale |
|----------|----------------|-------------------|-----------|
| Dual rendering layers | GL mesh + ImGui overlay | Simplicity | Separates background geometry (GL, GPU-efficient) from text/interaction (ImGui, CPU-side). The layers are independent and composable. |
| Fixed 40 px cell size | DPI-scaled fixed cell | Resolution-independent proportion | Fixed physical size in pixels ensures consistent touch targets and readability. DPI scaling preserves physical size on Retina. |
| 8 segments per corner | Visual quality vs. performance | Negligible cost at low panel count | 8 arcs per quarter-circle produces smooth-looking corners. |
| Rebuild all panels on any change | Simplicity | Incremental update efficiency | With few panels, full rebuild is < 1ms. Per-panel dirty tracking would be premature. |
| `SectionLine` embedding all modes | Uniform iteration | Type safety for mutually exclusive modes | A single struct is simpler than a variant. Enforcement by convention. |
| Collapsed sections | State on `Section` element | Fully self-contained element | Collapse state is UI state; storing it on the element avoids a separate map but couples element definition to renderer behavior. |

---

## 7. Best Practices Compliance

### Conforming
- **RAII**: GL resources cleaned in destructors. Move semantics handle ownership transfer.
- **Dirty flag optimization**: GL mesh rebuilt only when `dirty = true`. ImGui overlay rebuilt every frame (ImGui is immediate-mode by design).
- **Correct blending**: `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA` with depth test disabled for 2D overlay.
- **Pointer stability**: `std::deque` for `RootPanel` ensures anchor pointers remain valid.
- **DPI scaling**: `UIGrid::Update` multiplies all cell sizes by `displayScale`.
- **Hover/active feedback**: `UIStyle::DrawInputHoverTint` and `SectionLine::onClick` underline provide visual interactivity cues.

### Non-Conforming

| Issue | Severity | Details | Remediation |
|-------|----------|---------|-------------|
| Two update patterns | Minor | `Display` uses both `SetSectionVisible(id, id, bool)` (string lookup) and `uiResult->visible = value` (direct pointer). | Remove the string-based API once all callers are migrated to direct `Paragraph*` / `Section*` access. |
| `Section::collapsed` owns rendering state | Minor | Collapse state sits on the element; the renderer expands/collapses based on it, not the element itself. | Acceptable coupling for simple toggle; would need rethinking if sections become independently animated. |
| `UIRenderer::Render()` mixes GL and ImGui | Minor | Two rendering backends interleaved in one method. | Consider a `RenderBackground()` / `RenderOverlay()` split for clarity, though not strictly necessary at current scale. |
| Forward-only anchor dependency | Minor | `ResolveAnchors` makes a single linear pass — panels can only anchor to earlier-added panels. Cyclic anchors silently produce wrong layouts. | Document the constraint. Add a debug assertion or cycle-detection pass if layout bugs are reported. |

---

## 8. Risks & Future-Proofing

### Risks

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| Two-layer synchronization | Medium — if panel bounds are stale when ImGui overlay is drawn, text positions will not match background rects | Low | Both layers are resolved in the same `Render()` call; stale positions require a bug in the dirty-flag logic. |
| ImGui version coupling | Low — ImGui API is stable but font/DrawList calls could change | Low | Use a pinned ImGui version; abstract `iconDraw` signatures are ImGui-internal. |
| Many panels | Low — mesh rebuild and ImGui window creation are O(panels × lines) | Low | Current UI has ~3 panels; add profiling if panel count grows significantly. |

### Future Considerations
- **Remove string-based `SetSectionVisible`/`SetSectionValue`** — all callers should use direct `Paragraph*` / `Section*` pointers.
- **Scrollable panels** — `Section` children overflow their parent for long result lists; a scroll container with clipping would be needed.
- **Panel animation** — slide-in/fade transitions would require per-frame interpolation and dirty-flag updates.
- **Tooltip support** — hovering over a `SectionLine` with `onClick` shows a tooltip. Already has ImGui hover detection; needs a text field on `SectionLine`.
- **Theming** — `Color` module already provides theme-aware colors; `UIStyle` centralizes widget style. Extend `UIStyle` for panel background gradients or shadow if needed.

---

## 9. Recommendations

### Should-Have
1. **Migrate all string-based lookups** — remove `SetSectionVisible` / `SetSectionValue` once all callers use direct `Paragraph*` / `Section*` access. The string API is a footgun.
2. **Scrollable section container** — long analysis result lists will overflow panels without a scroll region.

### Nice-to-Have
3. **`RenderBackground()` / `RenderOverlay()` split** — separate the GL mesh pass and ImGui overlay pass for code clarity.
4. **Tooltip support** — add an optional `tooltip` string to `SectionLine`, shown on hover via `ImGui::SetTooltip`.
5. **Panel removal API** — `RemovePanel(id)` for dynamic UI updates, currently only `visible` flag is used.
