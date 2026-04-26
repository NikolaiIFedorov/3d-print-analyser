# Architecture: Analysis — 3D-Printability Flaw Detection

## Executive Summary

The Analysis module is a plugin-style framework for detecting additive-manufacturing flaws in B-Rep geometry. It defines three abstract interfaces (`IFaceAnalysis` for per-face checks, `ISolidAnalysis` for per-solid layer-based checks, and `IEdgeAnalysis` for per-edge checks) and ships five concrete analyses: **Overhang**, **ThinSection**, **SmallFeature**, **Stringing**, and **SharpCorner**. A cross-sectional slicing utility (`Slice`) underpins three of the five analyses. Results are collected into an `AnalysisResults` struct that the `AnalysisRenderer` consumes. The module is ~600 lines across 14 files.

**Verdict: Approve with changes**

**Top 3 strengths:**
1. Extensible plugin architecture — adding a new analysis requires one class and one `Add*Analysis()` call. No existing code changes.
2. Clean interface segregation — `IFaceAnalysis` (returns single `FaceFlawKind`), `ISolidAnalysis` (returns `vector<FaceFlaw>`), and `IEdgeAnalysis` (returns `vector<EdgeFlaw>`) match three distinct analysis patterns.
3. `AnalysisResults` struct decouples analysis computation from rendering — analysis runs once, results are consumed by the renderer.

**Top 3 concerns:**
1. `Slice::At()` pairs intersections sequentially (`i, i+1`), which is incorrect for non-convex cross-sections or faces with >2 edge crossings per loop.
2. `Analysis` is a Meyer's singleton — hidden global state that couples rendering to analysis and prevents unit testing with different configurations.
3. Slicing (Z-bounds scan + `Slice::Range`) is duplicated across three analyses (`ThinSection`, `SmallFeature`, `Stringing`), causing redundant O(V) vertex scans per solid.

---

## 1. Requirements & Motivation

### Functional Requirements
- Detect five categories of 3D-printing flaws:
  - **Overhang**: faces whose surface normal exceeds the maximum self-supporting angle.
  - **Thin Section**: vertical runs of narrow cross-sections where height/width ratio exceeds a threshold.
  - **Sharp Corner**: edges between two planar faces where the dihedral angle is below a threshold.
  - **Stringing**: layers with more distinct contour loops than expected, indicating print-head stringing.
  - **Small Feature**: cross-section segments or widths below the minimum printable feature size.
- Produce per-face flaw classification (for face overlay coloring), per-solid face flaw ranges (for line/surface rendering), per-solid edge flaws, and bridge surfaces.
- Support user-extendable analysis via abstract interfaces.

### Non-Functional Requirements
- Analysis completes in interactive time for small-to-medium models (< 100ms for ~10k faces).
- Deterministic results — same geometry always produces the same flaws.

### Constraints
- Analyses operate on the Scene's B-Rep data (faces, edges, vertices).
- Layer-based analyses use horizontal (Z-constant) slicing.
- Analysis results must be consumable by the rendering pipeline without coupling the renderer to analysis internals.

---

## 2. Solution Description

### Components

| Component | Files | Responsibility |
|-----------|-------|----------------|
| **Analysis** | `Analysis.hpp/cpp` | Singleton registry. Stores `IFaceAnalysis`, `ISolidAnalysis`, and `IEdgeAnalysis` implementations. Runs all analyses and collects `AnalysisResults`. Computes shared `ZBounds` once per solid for solid analyses. |
| **IFaceAnalysis** | `Analysis.hpp` | Interface: `Analyze(Face*) → optional<FaceFlawKind>`. First match wins. |
| **ISolidAnalysis** | `Analysis.hpp` | Interface: `Analyze(Solid*, optional<ZBounds>, BridgeSurface*) → vector<FaceFlaw>`. All results aggregated. |
| **IEdgeAnalysis** | `Analysis.hpp` | Interface: `Analyze(Solid*) → vector<EdgeFlaw>`. All results aggregated. |
| **AnalysisTypes** | `AnalysisTypes.hpp` | Shared types: `FaceFlawKind` enum, `EdgeFlawKind` enum, `Segment`, `Layer`, `ZBounds`, `FaceFlaw`, `EdgeFlaw`, `BridgeSurface`, `AnalysisResults`. |
| **Overhang** | `Overhang.hpp/cpp` | **IFaceAnalysis**. Checks face normal Z-component against a threshold derived from `maxAngleDeg`. → `FaceFlawKind::OVERHANG`. |
| **ThinSection** | `ThinSection.hpp/cpp` | **ISolidAnalysis**. Slices the solid, measures `MinWidth` per layer, tracks runs of narrow layers. If `height/width ≥ heightToWidthRatio` → `FaceFlawKind::THIN_SECTION`. |
| **SharpCorner** | `SharpCorner.hpp/cpp` | **IEdgeAnalysis**. Iterates edges shared by exactly 2 planar faces. If dihedral cosine < threshold → `EdgeFlawKind::SHARP_CORNER`. |
| **Stringing** | `Stringing.hpp/cpp` | **ISolidAnalysis**. Slices the solid, counts distinct contour loops per layer. Layers with more loops than `maxContourCount` → `FaceFlawKind::STRINGING`. |
| **SmallFeature** | `SmallFeature.hpp/cpp` | **ISolidAnalysis**. Slices the solid, flags layers where segments are individually short or cross-section `MinWidth` is below minimum. → `FaceFlawKind::SMALL_FEATURE`. Optionally uses a `SharpCorner*` to skip already-flagged corner edges. |
| **Slice** | `Slice.hpp/cpp` | Utility: `At(solid, z)` → cross-section segments. `Range(solid, zMin, zMax, step)` → layers. `MinWidth(segments)` → narrowest cross-section distance. `GetZBounds(solid)` → Z extent. |

### Data Flow

```
Analysis::AnalyzeScene(scene)
├── for each Solid:
│   ├── for each Face in Solid:
│   │   └── IFaceAnalysis::Analyze(face) → optional<FaceFlawKind>
│   │       └── results.faceFlaws[face] = flaw (first match, or NONE)
│   │
│   ├── Analysis::FlawSolid(solid, &bridgeSurfaces)
│   │   ├── Slice::GetZBounds(solid) → {zMin, zMax}   [computed once]
│   │   └── for each ISolidAnalysis:
│   │       └── analysis->Analyze(solid, bounds, bridgeSurfaces)
│   │           ├── Slice::Range(solid, zMin, zMax, layerHeight)
│   │           │   └── for each z: Slice::At(solid, z) → segments
│   │           └── domain-specific flaw detection
│   │       → vector<FaceFlaw>  (aggregated into faceFlawRanges)
│   │
│   └── Analysis::FlawEdges(solid)
│       └── for each IEdgeAnalysis:
│           └── analysis->Analyze(solid)
│               └── iterate edges, compute dihedral angles
│           → vector<EdgeFlaw>  (stored in edgeFlaws[solid])
│
└── return AnalysisResults {
        faceFlaws,        // unordered_map<Face*, FaceFlawKind>
        faceFlawRanges,   // unordered_map<Solid*, vector<FaceFlaw>>
        edgeFlaws,        // unordered_map<Solid*, vector<EdgeFlaw>>
        bridgeSurfaces    // unordered_map<Solid*, vector<BridgeSurface>>
    }
```

### Slicing Algorithm (`Slice::At`)

For each face in the solid, for each edge loop:
1. Iterate edges. For each edge crossing the Z-plane: interpolate intersection point.
2. Collect all intersections for the loop.
3. Pair intersections sequentially: `(intersections[0], intersections[1])`, `(intersections[2], intersections[3])`, etc.

**Known defect**: Sequential pairing assumes exactly 2 intersections per loop (convex cross-sections). Non-convex faces or faces with multiple crossings per loop will produce incorrect segments.

### Key Abstractions

- **`IFaceAnalysis`**: Returns `optional<FaceFlawKind>` — first-match semantics. If no analysis returns a flaw, the face is `FaceFlawKind::NONE`.
- **`ISolidAnalysis`**: Returns `vector<FaceFlaw>` — all results are aggregated (a solid can have both thin-section and stringing flaws).
- **`IEdgeAnalysis`**: Returns `vector<EdgeFlaw>` — each carries the flawed `Edge*`, `EdgeFlawKind`, and Z-extent `ZBounds`.
- **`FaceFlaw`**: Richer than the old `Layer` — carries `face*`, `FaceFlawKind`, `ZBounds bounds`, and `clipBoundary` (a closed polygon used to clip the face color overlay to the affected region).
- **`EdgeFlaw`**: Carries `edge*`, `EdgeFlawKind`, and `ZBounds bounds` (the Z-extent of the flaw for rendering).
- **`BridgeSurface`**: Closed polygon boundary of a vertical connecting face — populated by solid analyses (currently unused by most analyses but reserved for bridge detection).
- **`AnalysisResults`**: Decoupling struct — analysis produces it, renderer consumes it. No back-reference to analysis logic. Four maps: `faceFlaws` (per face), `faceFlawRanges` (per solid, for range rendering), `edgeFlaws` (per solid), `bridgeSurfaces` (per solid).
- **`ZBounds`**: Shared Z-extent passed to solid analyses to avoid redundant vertex scanning.
- **`Layer`**: Slice result type — contains `segments` and optionally `triangles`, tagged with a `FaceFlawKind`.

---

## 3. Design Principles

### Conforming

| Principle | Evidence |
|-----------|----------|
| **SRP** | Each analysis class has exactly one flaw-detection job. `Slice` handles geometry intersection only. |
| **OCP** | Adding a new flaw type: create a class, register it in `Init()`. Zero changes to existing analyses or `Analysis`. |
| **LSP** | All `ISolidAnalysis` implementations are substitutable — `Analyze()` returns `vector<FaceFlaw>` regardless of flaw type. All `IEdgeAnalysis` implementations return `vector<EdgeFlaw>`. |
| **DIP** | `Analysis` depends on `IFaceAnalysis`/`ISolidAnalysis`/`IEdgeAnalysis` abstractions, not concrete implementations. Registration is via `unique_ptr`. |
| **ISP** | Three focused interfaces match three distinct analysis patterns: face-level, solid-level (slicing), and edge-level. |

### Deliberately Relaxed

| Principle | Deviation | Justification |
|-----------|-----------|---------------|
| **DIP** | `Analysis` is a singleton accessed via `Instance()` | Convenient global access from `Display::Frame()`. Acceptable in prototype; should inject for testability. |

---

## 4. Alternatives Considered

| Decision | Current Approach | Alternative | Verdict |
|----------|-----------------|-------------|---------|
| Analysis registration | Singleton with `AddFaceAnalysis/AddSolidAnalysis/AddEdgeAnalysis` | Dependency injection / constructor parameter | Singleton is simpler for now. **Inject for testability later.** |
| Slicing | Z-plane edge intersection, sequential pairing | Proper contour tracing (sort intersections along edge loop) | Sequential pairing is a correctness bug for non-convex geometry. **Must fix.** |
| Per-face flaw detection | First-match (`IFaceAnalysis` returns optional) | Priority-based (each returns severity, pick worst) | First-match is simpler. `Overhang` is currently the only face analysis. Revisit if multiple `IFaceAnalysis` implementations are needed. |
| SharpCorner | `IEdgeAnalysis` operating on edge graph | `ISolidAnalysis` with per-layer angle checks | Edge graph is the geometrically correct place to check dihedral angles — doesn't require slicing. **Current is better.** |
| Z-bounds sharing | Computed once in `FlawSolid()`, passed to analyses via `optional<ZBounds>` | Each analysis computes its own | **Current is correct** — shared bounds avoids redundant scans. |
| Result type for solid analyses | `vector<FaceFlaw>` (with `clipBoundary` and `ZBounds`) | `vector<Layer>` (segments only) | `FaceFlaw` provides richer information: face reference, Z-extent, and clip boundary for face overlay rendering. More renderable data per flaw. |

---

## 5. Technology & Dependencies

| Library | Role in Analysis |
|---------|-----------------|
| GLM | `glm::dvec3` / `glm::dvec2` for geometry math, `glm::dot`, `glm::normalize`, `glm::length` |

No external dependencies beyond the Scene module's geometry types.

---

## 6. Tradeoffs

| Decision | Chosen Approach | What's Sacrificed | Rationale |
|----------|----------------|-------------------|-----------|
| Hardcoded thresholds | Default constructor values (45°, 100°, 2.0mm, 0.4mm, 0.2mm layer height) | User configurability | Reasonable defaults for FDM printing. Constructors accept custom values; `Display` wires them to live DragFloat sliders. |
| Full re-slice per analysis | Each `ISolidAnalysis` calls `Slice::Range()` independently | Redundant slicing (3× for a solid) | `FlawSolid()` shares `ZBounds`, but slicing itself is repeated. Consolidate slicing to compute layers once and share. |
| Layer-based analysis | Fixed Z-step slicing | Adaptive slicing based on geometry features | Fixed step is predictable and simple. Adaptive would add complexity. |
| `SharpCorner` as `IEdgeAnalysis` | Examines edges with exactly 2 adjacent faces, computes dihedral angle | Slice-based angle detection | Edge graph approach is correct, fast, and doesn't require slicing. Moved from `ISolidAnalysis` to `IEdgeAnalysis` — correct classification. |
| `Stringing` uses contour count | Flags layers with more distinct loops than `maxContourCount` | Area/volume-based stringing metric | Contour count is a simple, geometry-independent signal for disconnected material regions. |

---

## 7. Best Practices Compliance

### Conforming
- **Plugin architecture**: Well-designed interface + registration pattern. Textbook OCP.
- **Optional return**: `IFaceAnalysis::Analyze` returns `optional<FaceFlawKind>` — clean no-match semantics.
- **Value semantics**: `FaceFlaw`, `EdgeFlaw`, `Layer`, `Segment`, `ZBounds` are simple value types — no ownership complexity.
- **Const correctness**: `Analyze()` methods are `const`; `Slice` methods are static.
- **Richer result types**: `FaceFlaw` carries `clipBoundary` and `ZBounds` so the renderer has all the data it needs without re-querying the analysis module.
- **Three-interface segregation**: `IFaceAnalysis`, `ISolidAnalysis`, `IEdgeAnalysis` each model a distinct analysis pattern; `SharpCorner` correctly moved to `IEdgeAnalysis`.

### Non-Conforming

| Issue | Severity | Details | Remediation |
|-------|----------|---------|-------------|
| `Slice::At()` sequential pairing | **Major** | `intersections[i], intersections[i+1]` is wrong for non-convex cross-sections. A face with 4 intersections at a Z-plane will produce paired segments that don't follow the actual contour. | Implement proper contour assembly: sort intersections along each edge loop's winding direction, then pair consecutive entry/exit points. |
| Singleton `Analysis::Instance()` | **Major** | Global mutable state. Cannot run different analysis configurations simultaneously. Cannot unit-test without the singleton. | Accept `const Analysis&` as a parameter to `Display::Frame()` and the renderer, or inject at construction time. |
| Redundant slicing | Minor | `ThinSection`, `SmallFeature`, and `Stringing` each call `Slice::Range()` independently for the same solid. | Compute layers once per solid in `FlawSolid()`, pass `const vector<Layer>&` to each `ISolidAnalysis::Analyze()`. |

---

## 8. Risks & Future-Proofing

### Risks

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| **Slice contour assembly** | High — non-convex solids produce incorrect results for 3 of 5 analyses | High (any real model with concavities) | Implement proper contour tracing in `Slice::At()` |
| **Performance at scale** | Medium — slicing is O(faces × edges × layers) | Medium (large models) | Cache slice results; parallelize per-solid analysis |
| **NURBS analysis** | Medium — `SharpCorner` operates on edge graph; face-level overhang is normal-based and correct for planar faces, but curved faces need normal sampling | Medium | Add NURBS normal sampling for face-level curvature-aware angle checks |

### Future Considerations
- **Configurable build direction**: Currently assumes Z-up. A configurable build vector would generalize analysis.
- **Analysis caching**: Cache `AnalysisResults` per solid, invalidate on geometry change.
- **Parallel analysis**: Independent analyses per solid are embarrassingly parallel — could use `std::async` or thread pool.
- **Support structure generation**: Current analysis detects problems; future work could suggest support placement.
- **Bridge detection**: `BridgeSurface` is already in `AnalysisResults` and populated; a full bridging analysis could use it to identify unsupported horizontal spans.

---

## 9. Recommendations

### Must-Have
1. **Fix `Slice::At()` contour assembly** — replace sequential pairing with proper contour tracing. Sort intersections along each edge loop's winding direction, then pair consecutive entry/exit points.

### Should-Have
2. **Decouple from singleton** — accept `const Analysis&` as parameter to `Display::Frame()` and renderer, or inject at construction time.
3. **Share sliced layers** — compute `Slice::Range()` once per solid in `FlawSolid()`, pass `const vector<Layer>&` to each `ISolidAnalysis::Analyze()`.

### Nice-to-Have
4. **Configurable build direction** — replace hardcoded Z-axis assumption with a `glm::dvec3 buildDirection` parameter.
5. **Analysis unit tests** — test with known geometries (45° ramp → overhang, thin wedge → thin section, single-loop layer → no stringing, etc.).
