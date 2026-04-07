# Architecture: Scene — B-Rep Geometry Kernel

## Executive Summary

The Scene module is the data-ownership layer of CAD_OpenGL. It implements a Boundary Representation (B-Rep) geometry kernel that models 3D objects as a topology graph of **Points → Edges → Faces → Solids**, with polymorphic Curve and Surface attachments for non-linear geometry. `std::deque` provides pointer-stable storage, and raw pointers form the dependency graph. The module is ~400 lines across 11 files.

**Verdict: Approve with changes**

**Top 3 strengths:**
1. Clean factory API (`Scene::Create*()`) centralizes entity construction and dependency wiring.
2. `std::deque` gives pointer stability without the indirection cost of `std::vector<unique_ptr>` — correct choice for a topology graph.
3. `OrientedEdge` decorator cleanly separates topological direction from geometric identity, keeping `Edge` itself direction-agnostic.

**Top 3 concerns:**
1. `NurbsSurface::GetNormal()` at the parametric midpoint is a reasonable default, but callers expecting a *face-representative* normal may get inconsistent results for highly curved surfaces.
2. `renderBuffer` / `lockedBuffer` are declared but never used — dead code.
3. No deletion or modification API exists yet — entities can only be created, not removed or edited.

---

## 1. Requirements & Motivation

### Functional Requirements
- Model 3D geometry as a B-Rep topology: points, edges (with optional curves), faces (with optional NURBS surfaces), and solids.
- Maintain a dependency graph so that higher-order entities (faces, solids) can trace back to constituent lower-order entities (edges, points).
- Support both planar and NURBS geometry for curves and surfaces.
- Provide a factory API for constructing entities with correct dependency wiring.

### Non-Functional Requirements
- Pointer stability: entities must not be invalidated when new entities are added.
- Minimal overhead: geometry is created once and read many times by rendering and analysis.

### Constraints
- C++23, GLM for math, TinyNURBS for NURBS evaluation.
- Solo developer; no concurrent access requirements.

---

## 2. Solution Description

### Components

| File | Type | Responsibility |
|------|------|----------------|
| `scene.hpp / .cpp` | Class `Scene` | Owns all entities in `std::deque` containers. Factory methods (`Create*`) construct entities and wire dependency pointers. |
| `Point.hpp` | Struct `Point` | 3D position (`glm::dvec3`) + back-pointers to dependent `Edge*` set. |
| `Edge.hpp` | Struct `Edge` | Connects two `Point*`, optional `Curve*`, optional `bridgePoints`. Back-pointers to dependent `Face*` set. |
| `OrientedEdge.hpp` | Struct `OrientedEdge` | Decorator: wraps `Edge*` + `bool reversed`. Provides `GetStart()/GetEnd()` respecting orientation. |
| `Curve.hpp / .cpp` | Abstract `Curve`, `ArcCurve`, `NurbsCurve` | Polymorphic curve evaluation. `Evaluate(t, start, end) → glm::dvec3`. Owned by `Scene::curves` as `unique_ptr<Curve>`. |
| `Surface.hpp` | Abstract `Surface`, `PlanarSurface`, `NurbsSurface` | Polymorphic normal query. `GetNormal()` / `GetNormal(u,v)` / `IsPlanar()`. Owned by `Face::surface` as `unique_ptr<Surface>`. |
| `Face.hpp / .cpp` | Class `Face` | Owns edge loops as `vector<vector<OrientedEdge>>`. Owns a `Surface` via `unique_ptr`. Auto-orients edge loops on construction. Computes planar data for planar faces. |
| `Solid.hpp` | Struct `Solid` | Aggregates `vector<Face*>`. Top-level entity. |
| `Geometry.hpp` | Structs `Vertex`, `ArcData`, `PlanarData` | Shared POD types used across modules. |
| `AllGeometry.hpp` | Meta-header | Includes all geometry headers + declares `FormPtr` variant. |

### Data Flow

```
Scene::CreatePoint(pos)
  → points.emplace_back(pos) → return &points.back()

Scene::CreateEdge(p0, p1)
  → edges.emplace_back(p0, p1)
  → p0->dependencies.insert(&edge)
  → p1->dependencies.insert(&edge)
  → return &edges.back()

Scene::CreateFace(edgeLoops)
  → faces.emplace_back(edgeLoops)
    → Face::OrientEdgeLoops() — determines reversed flag per edge
    → Face::CalculatePlanarData() — computes normal from first 3 vertices
  → edge->dependencies.insert(&face) for all edges
  → return &faces.back()

Scene::CreateSolid(faceVec)
  → solids.emplace_back()
  → face->dependency = &solid for all faces
  → return &solids.back()
```

### Key Abstractions

- **`Curve` hierarchy** — `ArcCurve` evaluates via angle interpolation around a center. `NurbsCurve` delegates to `tinynurbs::curvePoint()`. Both are substitutable via `Evaluate(t, start, end)`.
- **`Surface` hierarchy** — `PlanarSurface` stores a normal + offset. `NurbsSurface` wraps a `tinynurbs::RationalSurface3d` and computes normals via `tinynurbs::surfaceNormal()`.
- **`OrientedEdge`** — avoids duplicating edges for shared boundaries. A single `Edge` can appear in multiple face loops with different orientations.
- **Dependency graph** — upward: `Point → Edge → Face → Solid` via `unordered_set<T*> dependencies` and `Face::dependency`. This enables rendering to skip orphan entities (e.g., edges that belong to faces are rendered by the face, not independently).

---

## 3. Design Principles

### Conforming

| Principle | Evidence |
|-----------|----------|
| **SRP** | `Scene` owns storage and wiring. `Face` handles loop orientation and surface computation. `Curve`/`Surface` handle evaluation only. |
| **OCP** | New curve types (e.g., `BezierCurve`) or surface types can be added without modifying existing code. |
| **LSP** | `ArcCurve`/`NurbsCurve` are fully substitutable for `Curve`. `PlanarSurface`/`NurbsSurface` for `Surface`. |
| **DIP** | `Face` depends on the `Surface` abstraction, not concrete types. |
| **RAII** | `unique_ptr<Curve>` in `Scene::curves`, `unique_ptr<Surface>` in `Face::surface`. No raw new/delete. |

### Deliberately Relaxed

| Principle | Deviation | Justification |
|-----------|-----------|---------------|
| **ISP** | `Surface` has both `GetNormal()` and `IsPlanar()`, which are always used together. | Interface is minimal (3 methods). Splitting would add complexity for no benefit. |
| **Encapsulation** | `Scene` exposes `std::deque` containers as public members. | Renderers and analysis need to iterate all entities. A getter returning `const deque&` would be equivalent. Acceptable for this codebase size. |

---

## 4. Alternatives Considered

| Decision | Current Approach | Alternative | Verdict |
|----------|-----------------|-------------|---------|
| Entity storage | `std::deque<T>` per type + raw `*` graph | `std::vector<unique_ptr<T>>` | Deque gives pointer stability natively. Vector+unique_ptr adds indirection. **Current is correct.** |
| Curve ownership | `Scene::curves` owns via `deque<unique_ptr<Curve>>` | Embed `Curve` in `Edge` | Curves can be shared across edges (e.g., circle edge segments). Separate ownership is appropriate. |
| Face orientation | Computed in `Face` constructor | Pre-oriented by caller | Auto-orientation is more robust for imported geometry. **Current is good.** |
| Planar normal | Cross-product of first 3 loop vertices | Newell's method (robust for all polygon shapes) | Cross-product of 3 vertices can fail for degenerate first-3 configurations. **Should consider Newell's method for robustness.** |

---

## 5. Technology & Dependencies

| Library | Role in Scene Module |
|---------|---------------------|
| GLM | `glm::dvec3` for all positions and normals (double precision) |
| TinyNURBS | `tinynurbs::RationalCurve3d` / `RationalSurface3d` for NURBS storage; `curvePoint()` / `surfaceNormal()` for evaluation |

No new dependencies introduced.

---

## 6. Tradeoffs

| Decision | Chosen Approach | What's Sacrificed | Rationale |
|----------|----------------|-------------------|-----------|
| Double precision (`dvec3`) | Accuracy for CAD geometry | Memory (2× vs float), minor CPU cost | CAD tolerances require double precision. Rendering converts to `float` at upload time. |
| Raw pointer dependency graph | Simplicity, zero overhead | Safety (dangling pointers if deletion is added) | Scene owns all lifetime; no deletion API exists. If deletion is added, must invalidate dependents. |
| Public deque members | Direct iteration by renderers | Encapsulation | Pragmatic choice; adding const accessors later is trivial. |
| Auto-orientation in Face constructor | Robustness | Caller flexibility | Imported geometry (STL, OBJ) often has inconsistent winding. Auto-fix is valuable. |

---

## 7. Best Practices Compliance

### Conforming
- **Memory management**: `unique_ptr` for polymorphic types, deque for value types, no raw new/delete.
- **Naming**: `PascalCase` for types, `camelCase` for members, matches `best_practices.md`.
- **Null checks**: `CreateEdge` validates start/end points before construction.
- **Logging**: Every factory method logs its action via the `LOG_VOID` macro.

### Non-Conforming

| Issue | Severity | Details | Remediation |
|-------|----------|---------|-------------|
| `renderBuffer` / `lockedBuffer` unused | Minor | Declared in `Scene` but never read or written | Remove |
| `FormPtr` variant declared but unused | Minor | `using FormPtr = std::variant<...>` in `AllGeometry.hpp` | Remove or use |
| Planar normal from 3 vertices only | Minor | `CalculatePlanarData()` uses `oe0`, `oe1`, `oe2` start points — can fail if first 3 are collinear | Use Newell's method or check for degeneracy |
| No `const` on factory return | Minor | `CreatePoint()` returns `Point*` not `const Point*` | Acceptable — callers need mutation for dependency wiring |
| Edge wiring in `CreateEdge(start, end, curve)` calls `CreateEdge(start, end)` then mutates | Minor | Two-step construction; `curve->dependencies.insert(edge)` happens after initial creation | Could be combined but is functionally correct |

---

## 8. Risks & Future-Proofing

### Risks

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| Entity deletion | High — dangling pointers throughout dependency graph | Medium (will need this for model editing) | Implement cascading delete: removing a Point must remove dependent Edges, which must remove dependent Faces, etc. |
| NURBS face triangulation | High — Earcut assumes planar input | High (any real NURBS model) | Parametric-domain tessellation for NURBS faces, separate from Earcut path |
| Thread safety | Medium — renderers read while scene is modified | Low (currently single-threaded) | If multi-threading is added, snapshot or lock the scene during Frame() |

### Future Considerations
- **Entity IDs**: Adding stable integer IDs would enable serialization, undo/redo, and selection.
- **Deletion cascade**: When entity removal is needed, a `Scene::Delete*(T*)` that walks the dependency graph upward.
- **Scene diffing**: For incremental GPU upload, track which entities changed since last frame.
- **Shared edges**: Currently edges are duplicated per-face in mesh imports (STL/OBJ). Edge merging would reduce memory and enable manifold analysis.

---

## 9. Recommendations

### Must-Have
1. **Remove dead declarations** — `renderBuffer`, `lockedBuffer`, and `FormPtr` are unused.

### Should-Have
2. **Robustify `CalculatePlanarData()`** — handle collinear first-3-vertex cases via Newell's method or by searching for a non-degenerate triple.
3. **Add entity deletion** — even a simple cascade delete would enable model editing workflows.

### Nice-to-Have
4. **Entity IDs** — assign stable `uint32_t` IDs for serialization and selection.
5. **Edge merging** — for mesh imports, detect shared edges to build a proper manifold.
6. **Const accessors** — `const std::deque<T>& GetPoints() const` etc. to limit mutation surface.
