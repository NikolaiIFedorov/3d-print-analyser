# Architecture: Renderer — Display & Rendering Pipeline

## Executive Summary

The Renderer module handles all visual output in CAD_OpenGL. It is organized as a three-pass rendering pipeline — **SceneRenderer** (geometry wireframe + shaded patches), **AnalysisRenderer** (flaw overlays and analysis lines), and **UIRenderer** (2D panel overlays) — all composited into a single SDL3/OpenGL 3.3 framebuffer. The `Display` class orchestrates these renderers, owns the SDL window and GL context, manages the `Camera`, and drives the dirty-flag-based frame loop. The module is ~1,500 lines across 20+ files.

**Verdict: Approve with changes**

**Top 3 strengths:**
1. Clean three-pass architecture with separate renderers for scene, analysis, and UI — each independently uploadable and renderable.
2. Correct RAII and move semantics on all OpenGL resource classes (`OpenGLRenderer`, `AnalysisRenderer`, `UIRenderer`, `OpenGLShader`).
3. Dirty-flag frame loop (`cameraDirty` / `sceneDirty`) avoids redundant GPU work when nothing changes.

**Top 3 concerns:**
1. Full CPU-side geometry rebuild + GPU re-upload on every scene change will not scale beyond small models.
2. `Patch::AddFace` and `AnalysisRenderer::TriangulateFace` contain duplicated triangulation logic (~60 lines each).
3. `Display::InitUI()` hardcodes the entire UI layout — panel definitions, sizes, and button callbacks. This creates tight coupling between display and UI structure.

---

## 1. Requirements & Motivation

### Functional Requirements
- Render B-Rep geometry as shaded triangulated patches with depth-correct wireframe edges on top.
- Render analysis results as colored overlays (face highlights for overhang) and colored line segments (layer-based flaws).
- Render a 2D UI overlay for panels and buttons.
- Provide an orthographic camera with orbit, pan, zoom, roll, and scene-framing.
- Support window resizing with correct aspect ratio handling.

### Non-Functional Requirements
- Real-time interactive frame rates for small-to-medium models.
- Cross-platform via SDL3 + OpenGL 3.3 core profile.

### Constraints
- OpenGL 3.3 core profile (no fixed-function pipeline).
- SDL3 for windowing and GL context.
- Vertex-color shading only (no lighting).

---

## 2. Solution Description

### Components

| Component | Files | Responsibility |
|-----------|-------|----------------|
| **Display** | `display.hpp/cpp` | SDL3 window + GL context creation. Owns `SceneRenderer`, `AnalysisRenderer`, `UIRenderer`, `Camera`. Orchestrates frame loop with dirty flags. Delegates camera operations from Input. |
| **SceneRenderer** | `SceneRenderer.hpp/cpp` | Owns an `OpenGLRenderer`. Generates scene geometry via `Wireframe` and `Patch`, uploads to GPU, renders in a clear-draw-draw sequence. |
| **Wireframe** | `Wireframe.hpp/cpp` | Generates line mesh (vertices + indices) from Scene entities. Tessellates curved edges. Respects dependency hierarchy (edges owned by faces render with faces, not independently). |
| **Patch** | `patch.hpp/cpp` | Generates triangle mesh from Scene faces via Earcut 2D triangulation. Projects 3D face loops onto the face plane, triangulates, and emits vertex-colored triangles. |
| **AnalysisRenderer** | `AnalysisRenderer.hpp/cpp` | Separate GL pipeline with its own shader (`analysis.vert/frag`). Generates overlay triangles for flagged faces and colored line segments for analysis layers. Renders with blending enabled, depth test disabled. |
| **UIRenderer** | `UIRenderer.hpp/cpp` | 2D panel renderer using a grid-based layout. Builds rounded-rectangle meshes from `Panel` definitions. Renders with ortho projection, depth off, blend on. Handles hit-testing and click dispatch. |
| **OpenGLRenderer** | `OpenGLRenderer.hpp/cpp` | Low-level GL wrapper: manages VAO/VBO/IBO pairs for triangles and lines. Handles shader binding, matrix uniforms, depth state, polygon offset. |
| **OpenGLShader** | `OpenGLShader.hpp/cpp` | Compiles vertex+fragment shaders from files. Sets uniform values (`mat4`, `vec3`, `vec4`, `float`). Proper RAII with move semantics. |
| **Camera** | `camera.hpp/cpp` | Orthographic camera with quaternion-based orientation. Orbit (arcball), pan, zoom, roll, frame-bounds. Generates view and projection matrices. |
| **Color** | `color.hpp/cpp` | Centralized color constants and flaw-to-color mapping. Layered depth values for face/edge/point visual ordering. |
| **Shaders** | `basic.vert/frag`, `analysis.vert/frag`, `ui.vert/frag` | Three shader programs: basic (3D `vec3` position+color), analysis (3D `vec3` position + `vec4` color with alpha), UI (2D `vec2` position + `vec4` color). |

### Data Flow

```
Display::Frame()
├── if cameraDirty:
│   ├── SceneRenderer::SetCamera(camera)     → sets view/projection matrices
│   └── AnalysisRenderer::SetCamera(camera)  → sets viewProjection uniform
│
├── if sceneDirty:
│   ├── SceneRenderer::UpdateScene(scene)
│   │   ├── Wireframe::Generate() → line vertices + indices
│   │   │   └── OpenGLRenderer::UploadLineMesh()
│   │   └── Patch::Generate()     → triangle vertices + indices
│   │       └── OpenGLRenderer::UploadTriangleMesh()
│   │
│   └── if analysisEnabled:
│       ├── Analysis::Instance().AnalyzeScene(scene) → AnalysisResults
│       └── AnalysisRenderer::Update(scene, results)
│           ├── GenerateFaceOverlays() → overlay triangle mesh
│           └── GenerateLayerLines()   → colored line mesh
│
└── Display::Render()
    ├── SceneRenderer::Render()
    │   ├── OpenGLRenderer::Clear(background)
    │   ├── OpenGLRenderer::DrawTriangles()  (polygon offset ON)
    │   └── OpenGLRenderer::DrawLines()
    ├── AnalysisRenderer::Render()           (blend ON, depth OFF)
    └── UIRenderer::Render()                 (blend ON, depth OFF, ortho)
```

### Key Abstractions

- **Three-pass compositing**: Scene (opaque, depth-tested) → Analysis (transparent, depth-off) → UI (2D, depth-off). Rendering order ensures correct layering.
- **Dirty flags**: `cameraDirty` and `sceneDirty` prevent redundant uploads. Frame() is a no-op if nothing changed.
- **Polygon offset**: `glPolygonOffset(1.0, 1.0)` on triangle pass pushes faces behind edges, achieving clean wireframe-over-shaded rendering without z-fighting.
- **Vertex-color model**: No lighting — all color information is baked into vertices. Scene geometry uses `vec3` color, analysis uses `vec4` with alpha for transparency.

---

## 3. Design Principles

### Conforming

| Principle | Evidence |
|-----------|----------|
| **SRP** | `Wireframe` generates lines, `Patch` generates triangles, `Camera` handles projection, `OpenGLRenderer` handles GL state. Each has one job. |
| **OCP** | New renderers (e.g., for selection highlighting) can be added to `Display::Render()` without modifying existing renderers. |
| **RAII** | `OpenGLRenderer`, `AnalysisRenderer`, `UIRenderer`, `OpenGLShader` all clean up GL resources in destructors with move semantics. |
| **DIP** | `Patch` and `Wireframe` depend on Scene abstractions (`Face`, `Edge`, `Curve`), not concrete types. |

### Deliberately Relaxed

| Principle | Deviation | Justification |
|-----------|-----------|---------------|
| **DRY** | Triangulation code is duplicated between `Patch::AddFace` and `AnalysisRenderer::TriangulateFace` | Both need slightly different vertex types (`Vertex` vs `AnalysisVertex`). Could be templatized but adds complexity for two call sites. |
| **SRP** | `Display` handles window init, camera ops, UI init, and frame orchestration | Acceptable as a top-level coordinator. Splitting would add indirection without benefit at this scale. |

---

## 4. Alternatives Considered

| Decision | Current Approach | Alternative | Verdict |
|----------|-----------------|-------------|---------|
| Full re-upload per scene change | Simplicity | Dirty-tracking per entity, incremental upload | Current is fine for small models. Add dirty-tracking when scaling. |
| Three separate renderers | Clean separation | Single renderer with multiple passes | Separate renderers allow independent shaders and vertex formats. **Current is better.** |
| Vertex coloring | Simplicity | Phong/PBR lighting with normal data | Vertex coloring is sufficient for flaw visualization. Lighting can be added later with a new shader. |
| Orthographic projection only | CAD convention | Perspective option | Orthographic is standard for CAD. **Correct choice.** |
| Quaternion camera | Gimbal-lock-free orbiting | Euler angles | Quaternion is the right choice for unconstrained 3D rotation. |

---

## 5. Technology & Dependencies

| Library | Role in Renderer |
|---------|-----------------|
| SDL3 | Window creation, GL context, swap buffers, window events |
| GLAD | OpenGL 3.3 function loader |
| GLM | Matrix/vector math, quaternions, projections |
| Earcut | 2D polygon triangulation for face rendering |

### Shader Programs

| Shader | Vertex Format | Uniforms | Purpose |
|--------|--------------|----------|---------|
| `basic` | `vec3 position, vec3 color` | `mat4 uViewProjection, mat4 uModel` | Scene geometry (opaque) |
| `analysis` | `vec3 position, vec4 color` | `mat4 uViewProjection, mat4 uModel` | Flaw overlays (transparent) |
| `ui` | `vec2 position, vec4 color` | `mat4 uProjection` | 2D panels (screen-space) |

---

## 6. Tradeoffs

| Decision | Chosen Approach | What's Sacrificed | Rationale |
|----------|----------------|-------------------|-----------|
| Full geometry rebuild on scene change | Simplicity | GPU efficiency for large models | Scene changes are infrequent (load a file, toggle analysis). Acceptable for prototype. |
| Separate VAO/VBO per renderer | Isolation | Memory (3× VAO sets) | Each renderer has independent upload/draw lifecycle. Memory cost is negligible. |
| No lighting | Simplicity, flat shading makes flaw colors unambiguous | Visual quality | This is an analysis tool, not a modeler. Flaw visibility > aesthetics. |
| Polygon offset for edge rendering | Correct depth layering | Slight depth precision loss | Standard technique for wireframe-over-shaded. |
| `GL_DYNAMIC_DRAW` for all buffers | Correctness for changing data | Would be `GL_STATIC_DRAW` for static models | Scene data changes on import; dynamic hint is appropriate. |

---

## 7. Best Practices Compliance

### Conforming
- **RAII**: All GL resources cleaned in destructors; move semantics prevent double-free.
- **Depth management**: Correct use of `GL_LEQUAL`, polygon offset, depth enable/disable per pass.
- **Blending**: Analysis and UI renderers correctly enable/disable `GL_BLEND` with proper blend function.
- **Error checking**: `OpenGLRenderer::GetGLError()` uses `std::source_location` for precise error reporting.
- **Shader loading**: `OpenGLShader` validates compile and link errors with descriptive messages.

### Non-Conforming

| Issue | Severity | Details | Remediation |
|-------|----------|---------|-------------|
| Duplicated triangulation code | Minor | `Patch::AddFace` and `AnalysisRenderer::TriangulateFace` are ~90% identical | Extract a shared `TriangulateFace<VertexT>()` template or output generic data |
| `Display::InitUI()` hardcodes UI layout | Minor | Panel definitions, sizes, callbacks all inline in display.cpp | Move UI definition to a separate config or builder |
| `Display` includes 6 import headers | Minor | `STLImport.hpp`, `OBJImport.hpp`, `ThreeMFImport.hpp`, `FileImport.hpp`, `Analysis.hpp` in display.cpp | These are only needed for the file-open callback in `InitUI()` — could be in a separate handler |
| `OpenGLRenderer::EndFrame()` calls `SwapWindow()` | Minor | But `Display::Render()` also calls `SDL_GL_SwapWindow()` — double swap if `EndFrame` is accidentally called | `EndFrame()` appears unused in the actual render path; remove or clarify |
| Camera members are public | Minor | `target`, `distance`, `orientation`, `orthoSize`, etc. are all public | Acceptable for now but encapsulation would prevent invalid states |

---

## 8. Risks & Future-Proofing

### Known open defect: viewport depth vs geometry / pick

Some views show **internal or rear edges** and **mesh tessellation** faintly through solid
front faces, or allow **hover pick** of geometry that should be occluded. The same depth
class can affect **filled patches** (lighting, analysis-driven vertex colors, hover highlight
fills), not only wireframe lines — see `documentation/TODO` → **\[2026-04-27\] Viewport depth
& occluded edges**. Repro can be **intermittent**; faint ghost edges and small shifts under a
depth prepass experiment have been reported.

### Risks

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| Geometry rebuild cost | Medium — full CPU rebuild on every scene change | Medium (models grow) | Add per-entity dirty tracking; rebuild only changed sub-meshes |
| NURBS face rendering | High — Earcut cannot triangulate non-planar faces | High (real NURBS models) | Implement parametric-domain tessellation (sample UV grid) for NURBS faces |
| Single-threaded upload | Low — blocks frame during geometry generation | Low (currently fast) | Move geometry generation to background thread, double-buffer mesh data |

### Future Considerations
- **Lighting shader**: Adding a Phong pass would require normals in the vertex buffer. Plan the `Vertex` struct expansion (add `vec3 normal`).
- **Selection rendering**: A fourth pass with unique-color-per-entity for GPU picking.
- **Multi-solid visibility**: Per-solid show/hide would require splitting GPU buffers per solid.
- **Post-processing**: MSAA or FXAA for anti-aliasing would require framebuffer management.

---

## 9. Recommendations

### Must-Have
1. **Remove `OpenGLRenderer::EndFrame()`** or ensure it's not double-swapping with `Display::Render()`.

### Should-Have
2. **Extract shared triangulation** — `Patch::AddFace` and `AnalysisRenderer::TriangulateFace` should share the Earcut projection+triangulation logic.
3. **Move UI definition out of Display** — `InitUI()` creates tight coupling between the display coordinator and UI layout details.
4. **Decouple import logic from Display** — the file-open callback in `InitUI()` directly calls `STLImport::Import`, `OBJImport::Import`, etc. Move this to an application-level handler.

### Nice-to-Have
5. **Per-entity dirty tracking** — only regenerate and re-upload geometry for changed entities.
6. **camera encapsulation** — make members private, expose only semantic operations.
7. **Adaptive curve tessellation** — current fixed 16 segments per curve could be resolution-adaptive based on screen-space arc length.
