# Architecture: Analysis — 3D-Printability Flaw Detection

## Executive Summary

The Analysis module is a plugin-style framework for detecting additive-manufacturing flaws in B-Rep geometry. It defines two abstract interfaces (`IFaceAnalysis` for per-face checks, `ISolidAnalysis` for per-solid layer-based checks) and ships five concrete analyses: **Overhang**, **ThinSection**, **SharpCorner**, **Bridging**, and **SmallFeature**. A cross-sectional slicing utility (`Slice`) underpins four of the five analyses. Results are collected into an `AnalysisResults` struct that the `AnalysisRenderer` consumes. The module is ~600 lines across 14 files.

**Verdict: Approve with changes**

**Top 3 strengths:**
1. Extensible plugin architecture — adding a new analysis requires one class and one `AddSolidAnalysis()` call. No existing code changes.
2. Clean interface segregation — `IFaceAnalysis` (returns single `Flaw`) vs `ISolidAnalysis` (returns `vector<Layer>`) match the two distinct analysis patterns.
3. `AnalysisResults` struct decouples analysis computation from rendering — analysis runs once, results are consumed by the renderer.

**Top 3 concerns:**
1. `Slice::At()` pairs intersections sequentially (`i, i+1`), which is incorrect for non-convex cross-sections or faces with >2 edge crossings per loop.
2. `Analysis` is a Meyer's singleton — hidden global state that couples rendering to analysis and prevents unit testing with different configurations.
3. Slicing (Z-bounds scan + `Slice::Range`) is duplicated across four analyses, causing redundant O(V) vertex scans per solid.

---

## 1. Requirements & Motivation

### Functional Requirements
- Detect five categories of 3D-printing flaws:
  - **Overhang**: current-layer segments not supported by the layer below.
  - **Thin Section**: vertical runs of narrow cross-sections where height/width ratio exceeds a threshold.
  - **Sharp Corner**: edges between two planar faces where the dihedral angle is below a threshold.
  - **Bridging**: unsupported segments longer than a minimum bridge length.
  - **Small Feature**: cross-section segments or widths below the minimum printable feature size.
- Produce per-face flaw classification (for face overlay coloring) and per-solid layer segments (for line rendering).
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
| **Analysis** | `Analysis.hpp/cpp` | Singleton registry. Stores `IFaceAnalysis` and `ISolidAnalysis` implementations. Runs all analyses and collects `AnalysisResults`. Computes shared `ZBounds` once per solid for solid analyses. |
| **IFaceAnalysis** | `Analysis.hpp` | Interface: `Analyze(Face*) → optional<Flaw>`. First match wins. |
| **ISolidAnalysis** | `Analysis.hpp` | Interface: `Analyze(Solid*, optional<ZBounds>) → vector<Layer>`. All results aggregated. |
| **AnalysisTypes** | `AnalysisTypes.hpp` | Shared types: `Flaw` enum, `Segment`, `Layer`, `ZBounds`, `AnalysisResults`. |
| **Overhang** | `Overhang.hpp/cpp` | ISolidAnalysis. Slices the solid, compares each layer with the layer below using point-in-contour test. Unsupported segments → `Flaw::OVERHANG`. |
| **ThinSection** | `ThinSection.hpp/cpp` | ISolidAnalysis. Slices the solid, measures `MinWidth` per layer, tracks runs of narrow layers. If `height/width ≥ aspectThreshold` → `Flaw::THIN_SECTION`. |
| **SharpCorner** | `SharpCorner.hpp/cpp` | ISolidAnalysis. Iterates edges shared by exactly 2 planar faces. If dihedral cosine < threshold → `Flaw::SHARP_CORNER`. |
| **Bridging** | `Bridging.hpp/cpp` | ISolidAnalysis. Like Overhang but additionally filters by minimum bridge length. Unsupported long segments → `Flaw::BRIDGING`. |
| **SmallFeature** | `SmallFeature.hpp/cpp` | ISolidAnalysis. Slices the solid, flags layers where segments are individually short or cross-section `MinWidth` is below minimum. → `Flaw::SMALL_FEATURE`. |
| **Slice** | `Slice.hpp/cpp` | Utility: `At(solid, z)` → cross-section segments. `Range(solid, zMin, zMax, step)` → layers. `MinWidth(segments)` → narrowest cross-section distance. `GetZBounds(solid)` → Z extent. |

### Data Flow

```
Analysis::AnalyzeScene(scene)
├── for each Solid:
│   ├── for each Face in Solid:
│   │   └── IFaceAnalysis::Analyze(face) → optional<Flaw>
│   │       └── results.faceFlaws[face] = flaw
│   │
│   └── Analysis::FlawSolid(solid)
│       ├── Slice::GetZBounds(solid) → {zMin, zMax}   [computed once]
│       └── for each ISolidAnalysis:
│           └── analysis->Analyze(solid, bounds)
│               ├── Slice::Range(solid, zMin, zMax, layerHeight)
│               │   └── for each z: Slice::At(solid, z) → segments
│               └── domain-specific flaw detection
│           → results.solidLayers[solid] += layers
│
├── for each standalone Face (not in a Solid):
│   └── IFaceAnalysis::Analyze(face)
│
└── return AnalysisResults
```

### Slicing Algorithm (`Slice::At`)

For each face in the solid, for each edge loop:
1. Iterate edges. For each edge crossing the Z-plane: interpolate intersection point.
2. Collect all intersections for the loop.
3. Pair intersections sequentially: `(intersections[0], intersections[1])`, `(intersections[2], intersections[3])`, etc.

**Known defect**: Sequential pairing assumes exactly 2 intersections per loop (convex cross-sections). Non-convex faces or faces with multiple crossings per loop will produce incorrect segments.

### Key Abstractions

- **`IFaceAnalysis`**: Returns `optional<Flaw>` — first-match semantics. If no analysis returns a flaw, the face is `Flaw::NONE`.
- **`ISolidAnalysis`**: Returns `vector<Layer>` — all results are aggregated (a solid can have overhang AND thin-section layers).
- **`Layer`**: A set of `Segment`s at a Z-height with a `Flaw` tag. Renderable as colored lines.
- **`AnalysisResults`**: Decoupling struct — analysis produces it, renderer consumes it. No back-reference to analysis logic.
- **`ZBounds`**: Shared Z-extent passed to solid analyses to avoid redundant vertex scanning.

---

## 3. Design Principles

### Conforming

| Principle | Evidence |
|-----------|----------|
| **SRP** | Each analysis class has exactly one flaw-detection job. `Slice` handles geometry intersection only. |
| **OCP** | Adding a new flaw type: create a class, register it in `Init()`. Zero changes to existing analyses or `Analysis`. |
| **LSP** | All `ISolidAnalysis` implementations are substitutable — `Analyze()` returns `vector<Layer>` regardless of flaw type. |
| **DIP** | `Analysis` depends on `IFaceAnalysis`/`ISolidAnalysis` abstractions, not concrete implementations. Registration is via `unique_ptr`. |
| **ISP** | `IFaceAnalysis` (single-face → optional flaw) and `ISolidAnalysis` (whole-solid → layers) are cleanly separated interfaces matching their usage patterns. |

### Deliberately Relaxed

| Principle | Deviation | Justification |
|-----------|-----------|---------------|
| **DIP** | `Analysis` is a singleton accessed via `Instance()` | Convenient global access from `Display::Frame()`. Acceptable in prototype; should inject for testability. |
| **DRY** | `PointInsideContour()` is duplicated in `Overhang.cpp` and `Bridging.cpp` | Both files define an identical local static function. Should be in `Slice` utilities. |

---

## 4. Alternatives Considered

| Decision | Current Approach | Alternative | Verdict |
|----------|-----------------|-------------|---------|
| Analysis registration | Singleton with `AddFaceAnalysis/AddSolidAnalysis` | Dependency injection / constructor parameter | Singleton is simpler for now. **Inject for testability later.** |
| Slicing | Z-plane edge intersection, sequential pairing | Proper contour tracing (sort intersections along edge loop) | Sequential pairing is a correctness bug for non-convex geometry. **Must fix.** |
| Per-face flaw detection | First-match (`IFaceAnalysis` returns optional) | Priority-based (each returns severity, pick worst) | First-match is simpler. Overhang is currently the only face flaw. Revisit if multiple face flaws are needed. |
| Layer comparison for overhang | Point-in-contour test of current midpoints vs below-layer polygon | Area overlap / polygon boolean | Point-in-contour is O(n) per segment and correct for simple contours. Good pragmatic choice. |
| Z-bounds sharing | Computed once in `FlawSolid()`, passed to analyses via `optional<ZBounds>` | Each analysis computes its own | **Current is correct** — shared bounds avoids redundant scans. |

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
| Hardcoded thresholds | Default constructor values (45°, 1.5mm, 40°, 2.0mm, 0.8mm, 0.2mm layer height) | User configurability | Reasonable defaults for FDM printing. Constructors accept custom values. |
| Full re-slice per analysis | Each ISolidAnalysis calls `Slice::Range()` independently | Redundant slicing (4× for a solid) | `FlawSolid()` already shares `ZBounds`, but slicing itself is repeated. Consolidate slicing to compute layers once. |
| Point-in-contour for support detection | Simple ray-casting | Proper polygon containment with winding number | Ray-casting is correct for simple polygons and efficient. |
| Layer-based analysis | Fixed Z-step slicing | Adaptive slicing based on geometry features | Fixed step is predictable and simple. Adaptive would add complexity. |
| SharpCorner uses edge graph | Examines edges with exactly 2 adjacent faces | Examine all face pairs | Edge-based is more efficient and topologically correct. |

---

## 7. Best Practices Compliance

### Conforming
- **Plugin architecture**: Well-designed interface + registration pattern. Textbook OCP.
- **Optional return**: `IFaceAnalysis::Analyze` returns `optional<Flaw>` — clean no-match semantics.
- **Value semantics**: `Layer`, `Segment`, `ZBounds` are simple value types — no ownership complexity.
- **Const correctness**: `Analyze()` methods are `const`; `Slice` methods are static.
- **Parameter validation**: Analyses check for degenerate cases (e.g., `zMax - zMin < layerHeight * 2`) before proceeding.

### Non-Conforming

| Issue | Severity | Details | Remediation |
|-------|----------|---------|-------------|
| `Slice::At()` sequential pairing | **Major** | `intersections[i], intersections[i+1]` is wrong for non-convex cross-sections. A face with 4 intersections at a Z-plane will produce paired segments that don't follow the actual contour. | Implement proper contour assembly: sort intersections along each edge loop, or use winding-number-based inside/outside pairing. |
| Singleton `Analysis::Instance()` | **Major** | Global mutable state. Cannot run different analysis configurations simultaneously. Cannot unit-test without the singleton. | Accept `Analysis&` as a parameter or use dependency injection. |
| Duplicated `PointInsideContour()` | Minor | Identical function in `Overhang.cpp` and `Bridging.cpp` | Move to `Slice` as a static utility. |
| Redundant slicing | Minor | `Overhang`, `ThinSection`, `Bridging`, `SmallFeature` each call `Slice::Range()` independently for the same solid | Compute layers once per solid in `FlawSolid()`, pass to each analysis. |
| No `IFaceAnalysis` implementations registered | Minor | `faceAnalyses` vector is empty — `FlawFace()` always returns `Flaw::NONE`. Face flaws are only detected via solid analysis (overhang via layers). | If face-level flaw results are needed (currently used in `AnalysisResults::faceFlaws`), add an `OverhangFace` analysis using face normal angle check. |
| `Flaw` enum mixes face and solid concerns | Minor | `Flaw::OVERHANG` is used for both face overlays and layer lines | Acceptable for now — the same flaws apply at both levels. |

---

## 8. Risks & Future-Proofing

### Risks

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| **Slice contour assembly** | High — non-convex solids produce incorrect results for 4 of 5 analyses | High (any real model with concavities) | Implement proper contour tracing in `Slice::At()` |
| **Performance at scale** | Medium — slicing is O(faces × edges × layers) | Medium (large models) | Cache slice results; parallelize per-solid analysis |
| **NURBS analysis** | Medium — `SharpCorner` skips non-planar faces; overhang via layers works but face-level check doesn't exist | Medium | Add NURBS normal sampling for face-level angle checks |

### Future Considerations
- **Face-level overhang**: An `OverhangFace : IFaceAnalysis` that checks `face.GetSurface().GetNormal()` angle against build direction would complete the face-flaw pipeline.
- **Configurable build direction**: Currently assumes Z-up. A configurable build vector would generalize analysis.
- **Analysis caching**: Cache `AnalysisResults` per solid, invalidate on geometry change.
- **Parallel analysis**: Independent analyses per solid are embarrassingly parallel — could use `std::async` or thread pool.
- **Support structure generation**: Current analysis detects problems; future work could suggest support placement.

---

## 9. Recommendations

### Must-Have
1. **Fix `Slice::At()` contour assembly** — replace sequential pairing with proper contour tracing. Sort intersections along each edge loop's winding direction, then pair consecutive entry/exit points.

### Should-Have
2. **Decouple from singleton** — accept `const Analysis&` as parameter to `Display::Frame()` and renderer, or inject at construction time.
3. **Share sliced layers** — compute `Slice::Range()` once per solid in `FlawSolid()`, pass `const vector<Layer>&` to each `ISolidAnalysis::Analyze()`.
4. **Extract `PointInsideContour`** — move to `Slice` as a public static utility, remove duplicates from `Overhang.cpp` and `Bridging.cpp`.

### Nice-to-Have
5. **Add `OverhangFace : IFaceAnalysis`** — simple face-normal angle check would populate `faceFlaws` directly.
6. **Configurable build direction** — replace hardcoded Z-axis assumption with a `glm::dvec3 buildDirection` parameter.
7. **Analysis unit tests** — test with known geometries (45° ramp → overhang, thin wedge → thin section, etc.).
