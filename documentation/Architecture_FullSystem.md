# Architecture: CAD OpenGL — Full System Review

## Executive Summary

CAD_OpenGL is a 3D-printability analysis tool built on a B-Rep geometry kernel, OpenGL 3.3 rendering via SDL3, and a plugin-style analysis framework that detects manufacturing flaws (overhang, thin sections, sharp corners, bridging, small features). The codebase is ~2,500 lines of C++23 across four well-separated layers: **Scene / Geometry**, **Logic / Analysis**, **Display / Rendering**, and **Input**.

**Verdict: Approve with changes**

**Top 3 strengths:**
1. Clean layer separation — geometry, analysis, rendering, and input are distinct modules with minimal cross-contamination.
2. Extensible analysis framework — adding a new printability check requires a single class implementing `IFaceAnalysis` or `ISolidAnalysis`, registered in `Init()`.
3. Correct RAII and move semantics throughout OpenGL resource management.

**Top 3 concerns:**
1. The `Analysis` singleton creates a hidden global dependency that couples rendering directly to analysis logic and complicates testing.
2. Scene re-upload on every dirty frame re-generates *all* geometry CPU-side and re-uploads to the GPU, which will not scale beyond small models.
3. The slicing infrastructure (used by 4 of 5 analyses) recomputes the full Z-bounds scan for every analysis independently, resulting in redundant O(V) passes.

---

## 1. Requirements & Motivation

### Functional Requirements
- Model B-Rep geometry (points, edges, curves, faces, solids) with support for planar and NURBS surfaces.
- Render wireframe + shaded patches with depth-correct edge overlay.
- Detect additive-manufacturing flaws: overhang angles, thin sections, sharp corners, unsupported bridges, and small features.
- Navigate the 3D viewport via mouse (orbit, pan, zoom, roll) and trackpad gestures.

### Non-Functional Requirements
- Real-time interactive frame rates for small-to-medium models.
- Cross-platform (macOS primary target, Linux/Windows possible via SDL3 + CMake).
- C++23, OpenGL 3.3 core profile.

### Constraints
- Solo developer; no CI pipeline currently visible.
- Third-party libraries vendored in `include/`: GLM 1.0.2, TinyNURBS, Earcut, GLAD, GLFW headers (unused at runtime — SDL3 is the actual windowing backend).

---

## 2. Solution Description

### Components

| Layer | Module | Responsibility |
|-------|--------|----------------|
| **Scene** | `Scene`, `Point`, `Edge`, `Face`, `Solid`, `Curve`, `Surface`, `OrientedEdge` | B-Rep topology & geometry ownership. `std::deque` stores entities; raw pointers form the dependency graph. |
| **Analysis** | `Analysis` (singleton), `IFaceAnalysis`, `ISolidAnalysis`, `Overhang`, `ThinSection`, `SharpCorner`, `Bridging`, `SmallFeature`, `Slice` | Stateless flaw-detection algorithms registered at startup. Per-face and per-solid analysis pipelines. |
| **Rendering** | `Display`, `SceneRenderer`, `Patch`, `Wireframe`, `OpenGLRenderer`, `OpenGLShader`, `Camera`, `Color` | Earcut triangulation, curve tessellation, GPU upload (VBO/VAO/IBO), orthographic camera, depth-correct edge rendering. |
| **Input** | `Input` | SDL3 event dispatch; mouse and multi-finger trackpad gesture classification (pan vs zoom vs orbit). |

### Data Flow

```
Scene::Create*()  →  scene.{points,edges,faces,solids} (std::deque)
         ↓
Display::UpdateScene()  →  SceneRenderer::UpdateScene()
         ↓                         ↓
   Wireframe::Generate()      Patch::Generate()
   (+ Analysis::FlawSolid)    (+ Analysis::FlawFace via Color)
         ↓                         ↓
   OpenGLRenderer::UploadLineMesh / UploadTriangleMesh
         ↓
   OpenGLRenderer::DrawTriangles + DrawLines → SwapWindow
```

### Key Abstractions
- **`Surface` / `Curve`** — virtual base classes with `PlanarSurface`/`NurbsSurface` and `ArcCurve`/`NurbsCurve` implementations. Well-separated.
- **`IFaceAnalysis` / `ISolidAnalysis`** — analysis plugin interfaces. Each returns either a single `Flaw` or a vector of `Layer` segments.
- **`OrientedEdge`** — decorator around `Edge*` that handles topological reversal, keeping the `Edge` itself direction-agnostic.
- **`FormPtr` (`std::variant<Point*, Edge*, Curve*, Face*, Solid*>`)** — declared but not yet used in the codebase.

---

## 3. Design Principles

### Conforming

| Principle | Evidence |
|-----------|----------|
| **SRP** | Each analysis class has exactly one job. `Wireframe`, `Patch`, `Camera`, `OpenGLRenderer` each own one concern. |
| **OCP** | New flaw types are added by creating a class + registering it; no existing code changes. |
| **LSP** | `PlanarSurface`/`NurbsSurface` are correctly substitutable for `Surface`. Same for `Curve` hierarchy. |
| **DIP** | Analysis framework depends on `IFaceAnalysis`/`ISolidAnalysis` abstractions, not concrete classes. |
| **RAII** | `OpenGLRenderer` and `OpenGLShader` have proper move semantics and cleanup in destructors. `std::unique_ptr` for `Curve` and `Surface` ownership. |

### Deliberately Relaxed

| Principle | Deviation | Justification |
|-----------|-----------|---------------|
| **DIP** (rendering → analysis) | `Color::GetFace()` and `Wireframe::AddSolid()` call `Analysis::Instance()` directly | Convenient for now; the analysis results are needed at render-time. Acceptable in a prototype, but should be decoupled for testability. |
| **ISP** | `Surface::GetNormal()` is the only method; `IsPlanar()` is a type query. | Minimal interface — not yet problematic, but NURBS `GetNormal()` returning a hardcoded `(0,0,1)` is a correctness gap. |

---

## 4. Alternatives Considered

| Decision | Current Approach | Alternative | Tradeoffs |
|----------|-----------------|-------------|-----------|
| **Entity storage** | `std::deque` per type + raw `*` graph | ECS (entt), or `std::vector<unique_ptr>` | Deque gives pointer stability for free; raw pointers are fine since Scene owns lifetimes. ECS would be over-engineering at this scale. **Current approach is appropriate.** |
| **Analysis invocation** | Singleton `Analysis::Instance()` called from rendering | Pass analysis results as a parameter to `Generate()` | Decouples rendering from analysis; enables unit testing without a singleton. **Recommended change.** |
| **Triangulation** | Earcut (2D polygon earclipping) | CGAL constrained Delaunay, libtess2 | Earcut is header-only, fast, handles holes. Correct choice for planar faces. NURBS faces will need parametric-domain tessellation — Earcut won't generalize there. |
| **Windowing** | SDL3 | GLFW (already vendored in `include/`) | SDL3 gives better touch/trackpad support and is more actively maintained. Good choice. The vendored GLFW headers are unused dead weight. |
| **Slicing** | Each analysis independently scans Z-bounds and calls `Slice::Range()` | Pre-compute slices once per solid, share across analyses | Would eliminate 3× redundant vertex scans and slice computations. **Recommended change.** |

---

## 5. Technology & Dependencies

### Current Dependencies

| Library | Version | License | Role | Notes |
|---------|---------|---------|------|-------|
| SDL3 | system (find_package) | zlib | Windowing, GL context, input | Good choice; actively maintained |
| GLAD | vendored | MIT | OpenGL loader | Appropriate |
| GLM | 1.0.2 | MIT | Math | Standard choice |
| Earcut | 2.2.4 | ISC | 2D triangulation | Header-only, minimal |
| TinyNURBS | vendored | MIT | NURBS evaluation | Lightweight; used for curve/surface eval |
| GLFW | 3.4 (vendored) | zlib | **Unused at runtime** | Headers included in `Camera.hpp` but SDL3 is the actual backend. **Should be removed.** |

### Integration Impact
- `CMakeLists.txt` duplicates the source list (once in `set(SOURCES ...)`, again in `add_executable`). The `SOURCES` variable is never referenced — only the inline list is used.
- `camera.cpp` is listed twice in the source list.
- Include path `${DMAKE_SOURCE_DIR}` (typo for `${CMAKE_SOURCE_DIR}`) for earcut is silently broken.

---

## 6. Tradeoffs

| Decision | Chosen Approach | What's Sacrificed | Rationale |
|----------|----------------|-------------------|-----------|
| Full re-upload on scene change | Simplicity | GPU efficiency for large models | Acceptable for prototype; geometry changes are infrequent |
| Analysis at render time | Convenience (flaw colors appear automatically) | Testability, separation of concerns | Works for now; decouple when adding unit tests |
| Orthographic-only camera | Simplicity | Perspective view | CAD convention; appropriate for the domain |
| `std::deque` + raw pointers | Pointer stability without indirection | Cache locality (vs `std::vector`) | Correct tradeoff — topology graph needs stable addresses |
| Per-vertex color (no lighting) | Simplicity | Visual quality | Flaw visualization through color-coding is the priority, not photorealism |
| Hardcoded analysis parameters | Quick iteration | User configurability | Thresholds (45°, 1.5mm, 40°, 2.0mm, 0.8mm) are reasonable defaults |

---

## 7. Best Practices Compliance

### Conforming
- **Memory management**: `std::unique_ptr` for polymorphic `Curve`/`Surface` ownership. No raw `new`/`delete` outside of `Scene` factory methods (which place into deques).
- **RAII**: OpenGL resources cleaned up in destructors with proper move semantics.
- **Naming**: Consistent PascalCase for types, camelCase for variables/methods, matching `best_practices.md`.
- **Rendering**: Uses VAO/VBO/IBO properly. Polygon offset for depth-correct edge rendering. `GL_DYNAMIC_DRAW` for buffers that change per scene update.
- **Input abstraction**: SDL3 events are translated to high-level camera operations, not leaked into scene/rendering code.

### Non-Conforming

| Issue | Severity | Details | Remediation |
|-------|----------|---------|-------------|
| **GLFW header in Camera** | Minor | `#include "GLFW/glfw3.h"` in `camera.hpp` — unused, SDL3 is the backend | Remove the include |
| **CMake `SOURCES` variable unused** | Minor | Defined but `add_executable` uses a second inline list | Use `${SOURCES}` in `add_executable`, remove duplicate list |
| **`camera.cpp` listed twice** | Minor | Duplicate in CMake source list | Remove the duplicate |
| **`${DMAKE_SOURCE_DIR}` typo** | Minor | Earcut include path silently broken (works anyway via `include_directories` fallback) | Fix to `${CMAKE_SOURCE_DIR}` |
| **Singleton `Analysis::Instance()`** | Major | Global mutable state; hard to test; couples rendering to analysis | Inject analysis results into `Wireframe::Generate()` and `Color::GetFace()` |
| **`NurbsSurface::GetNormal()` stub** | Major | Returns hardcoded `(0,0,1)` — overhang analysis will produce wrong results for NURBS faces | Implement proper NURBS normal evaluation (sample partial derivatives at centroid) |
| **Redundant Z-bounds computation** | Minor | `ThinSection`, `Bridging`, `SmallFeature` each independently scan all vertices for zMin/zMax | Extract into a shared utility or precompute in `Slice` |
| **`Slice::At()` pairing assumption** | Major | Intersections are paired sequentially (`i, i+1`), which breaks for non-convex cross-sections or faces with multiple intersection points per loop | Implement proper contour assembly (sort intersections along each edge loop, or use winding-number based inside/outside) |
| **Global variables in `main.cpp`** | Minor | `scene`, `window`, `display`, `input` are file-scope globals | Wrap in an `Application` class or move into `main()` scope |
| **`#include <thread>` in SceneRenderer.cpp** | Minor | Unused include | Remove |
| **`glm::mat4 test` global in OpenGLRenderer.cpp** | Minor | Debug leftover | Remove |
| **`renderBuffer` / `lockedBuffer` in Scene** | Minor | Declared but never used | Remove or implement |

---

## 8. Risks & Future-Proofing

### Risks

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| **NURBS rendering correctness** | High — NURBS faces won't triangulate properly with Earcut (needs parametric-domain tessellation) | High (will hit this as soon as real NURBS models are loaded) | Plan a parametric tessellation path for NURBS faces separate from the planar Earcut path |
| **Slice contour assembly** | High — non-convex solids will produce incorrect thin-section/bridging/small-feature results | Medium | Replace sequential intersection pairing with proper contour tracing |
| **Scalability** | Medium — full CPU-side geometry rebuild + GPU re-upload on every scene change | Low (models are currently small) | Add dirty-tracking at the entity level; only re-upload changed geometry |
| **No test infrastructure** | Medium — analysis logic is algorithmically complex but has no automated tests | High | Add a test target in CMake; the analysis classes are already well-factored for unit testing |

### Future Considerations

- **File I/O**: No import/export yet (STEP, STL, etc.). The B-Rep model is built programmatically in `main()`. When file loading is added, the `Scene::Create*()` factory API is well-positioned to be the target.
- **Undo/Redo**: The current architecture (deque ownership + raw pointer graph) would need augmentation. Consider a command pattern on top of `Scene`.
- **Multi-solid scenes**: Analysis currently runs per-solid at render time. For scenes with many solids, consider caching analysis results and invalidating on geometry change.
- **Lighting / materials**: The current pass-through vertex-color shader is a dead end for visual quality. Adding a second shader with Phong/PBR lighting would require normal data in the vertex buffer — plan the `Vertex` struct expansion now.

---

## 9. Recommendations

### Must-Have
1. **Fix `Slice::At()` contour assembly** — the sequential pairing of intersections (`i, i+1`) is incorrect for non-convex geometry. This affects `ThinSection`, `Bridging`, and `SmallFeature` accuracy.
2. **Implement `NurbsSurface::GetNormal()`** — the hardcoded stub will silently produce wrong overhang results for any non-planar face.

### Should-Have
3. **Decouple analysis from rendering** — pass analysis results (flaw map, layers) into `Wireframe::Generate()` and `Patch::Generate()` rather than calling `Analysis::Instance()` from inside the render path. Enables unit testing and removes global state.
4. **Consolidate Z-bounds / slicing** — compute slices once per solid and share across `ThinSection`, `Bridging`, and `SmallFeature`.
5. **Clean up CMakeLists.txt** — remove duplicate source list, fix `DMAKE_SOURCE_DIR` typo, remove duplicate `camera.cpp` entry.
6. **Remove dead code** — unused GLFW include, `glm::mat4 test` global, unused `#include <thread>`, unused `FormPtr`, unused `renderBuffer`/`lockedBuffer`.

### Nice-to-Have
7. **Add a CMake test target** with basic analysis unit tests (a planar face at 60° should be flagged as overhang; a thin pyramid should produce thin-section layers).
8. **Dirty-tracking per entity** — only re-generate and re-upload geometry that changed, rather than rebuilding the entire mesh.
9. **Extract `Application` class** — wrap the globals in `main.cpp` (`scene`, `display`, `input`) into a single owning struct.
10. **Plan NURBS face tessellation** — Earcut operates in 2D and assumes planar input; NURBS faces will need parametric-domain sampling with adaptive refinement.
