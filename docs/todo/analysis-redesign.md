# Analysis Tool — Redesign

## Implementation status (2026-06-24)

Implemented: `Slice::ExtractLoops`, `GeometryOps::ConvexHull2D`/`RingBoolean`, the four new
detectors (`OverhangDetector`, `NotEnoughSpace`, `Instability`, `LayerDifference`), the
`Analysis`/`AnalysisTypes` pipeline rewrite, and all downstream consumers (`color.cpp`,
`AnalysisRenderer`/`patch.cpp`/`Wireframe.cpp`, `display.cpp`/`.hpp`, `Settings.hpp`,
`SessionLogger`, `Icons.hpp`, `test_structure_accept.cpp`). Both `CAD_OpenGL` and
`test_structure_accept` build clean.

**Known follow-up — OCCT boolean cost on continuously-changing cross-sections.** Overhang and
Layer Difference route their per-layer-pair polygon difference through OCCT (`GeometryOps::
RingBoolean`), per the accepted tradeoff in this doc. Profiling via `test_structure_accept`'s
`slanted-wedge` scenario (a prism with a continuously sloping top, ~28mm tall) found this
genuinely slow — `AnalyzeScene` stalled with memory climbing past 700MB on that solid
specifically, confirming the originally-accepted risk ("OCCT booleans are heavier... revisit
only if profiling shows this is a real bottleneck").

Mitigations added so far (all in `LayerDiffUtils`, used by both detectors):
- `LayersAreEffectivelyIdentical` — cheap pure-glm area/centroid pre-filter; skips the OCCT call
  entirely for the common case of an unchanged vertical-wall cross-section between layers.
- A ~1mm comparison stride (instead of comparing every single sliced layer) — bounds OCCT call
  count independent of how fine `layerHeight` is.
- `AnyLoopIsDegenerate` — skips the OCCT call when either layer's cross-section is already
  thinner than ~0.05mm (e.g. the tapering tip of a wedge), where OCCT's boolean algorithms are
  known to struggle and where Instability/Not Enough Space's own (non-OCCT) width checks already
  cover the same region.

These measurably helped (the test suite now makes steady progress through scenarios that
previously made zero progress in 2+ minutes), but the slanted-wedge scenario specifically was
not fully root-caused before time ran out on this pass — it's not yet confirmed whether the
remaining cost is concentrated in a few pathological layers or spread across many moderately
slow ones.

**Planned optimization (helps the common case, does not address this).** Generalize
`LayersAreEffectivelyIdentical` from a pairwise skip-the-boolean-call check into actually
collapsing a run of identical layers into one representative layer + a height span, before any
detector runs. This benefits more than Overhang/Layer Difference: Not Enough Space's
loop/corner score and Instability's run-detection are both pure functions of a layer's
cross-section, so a run of unchanged layers can be scored once instead of once per layer. Real
parts are dominated by straight-wall sections, so this should be a meaningful win in practice —
but it contributes nothing to the slanted-wedge case specifically, since a continuously-sloping
cross-section has no duplicate layers to collapse by construction. Don't treat this as a fix for
the open risk below; it's a separate, complementary optimization.

**Next step for the open risk — profile before picking a fix.** Get per-call timing across the
wedge's full height (not just a partial trace). The answer determines which of two very
different fixes is correct, so this should happen before either is attempted:
- **If cost is concentrated in a few pathological layers** (e.g. near-degenerate loops where
  OCCT's tolerance-based kernel does excessive retry work) — targeted fix: extend
  `AnyLoopIsDegenerate` to catch whatever specific geometric condition triggers the slow calls,
  the same way it already catches sub-0.05mm tapering tips. Cheap, low-risk, keeps OCCT as the
  only boolean implementation in the codebase.
- **If cost is broadly distributed** (every layer pays meaningful OCCT call overhead, with no
  duplicates to skip since the cross-section changes every layer) — no amount of caching or
  pre-filtering fixes this, because the cost is inherent to invoking a general-purpose,
  tolerance-robust 3D boolean kernel for what's fundamentally a simple 2D polygon-against-polygon
  operation. This is when the previously-declined alternative becomes justified: a hand-rolled
  2D polygon clipper scoped to simple (non-self-intersecting) loops, used only for this specific
  diff primitive (Overhang/Layer Difference's `currentLayer − previousLayer`). It would not
  touch OCCT usage anywhere else (Structure's carve, Calibrate's classification) — scoped
  strictly to this one hot path.

Building the clipper without the profiling data first risks solving the wrong problem if the
real cost turns out to be concentrated in a handful of calls rather than spread evenly.

## Why

The current pipeline splits detectors across three shapes — `IFaceAnalysis` (Overhang),
`ISolidAnalysis` (Small Feature, Thin Section, Stringing), `IEdgeAnalysis` (Sharp Corner) —
and several have real bugs:

- `ChainOuter` (`ThinSection.cpp`) can't separate multiple disjoint loops in one layer. It
  greedily stitches every non-hole segment into a single polygon regardless of topology, so
  any cross-section with more than one island gets a bogus merged polygon, corrupting its
  area/perimeter (and therefore `wEff`).
- `SmallFeature`'s `s1.face == s2.face` exclusion structurally can't detect a small feature
  whose entire boundary belongs to one face — e.g. a round pin/boss/hole, since every segment
  pair at that layer shares the same face.
- Thin Section's `2A/P` "effective width" is isotropic — it can't distinguish a stable wide
  panel from an unstable rod of the same average width.
- Overlapping detectors (Small Feature vs. Thin Section) can flag the same region for two
  different reasons with no precedence/merge.
- Stringing is fully implemented but never wired into `RebuildDefaultAnalyzers`.

Rather than patch each bug independently, we re-derived the detector list from the actual
physical causes of FDM defects and redesigned around that.

## Root-cause taxonomy

Process step → side effect → outcome:

- **Heating** → not uneven heating itself, but differential cooling/shrinkage after
  deposition (warping). Detectable as a jump in cross-sectional area between adjacent layers.
  Distinct from reheat below: shrinkage is a *global, uniform* effect — it shrinks the whole
  part by roughly the same proportion, which is exactly why a single scale factor (Calibrate's
  contour/hole/elephant's-foot compensation) corrects it. It does not explain feature-local
  defects, because its magnitude doesn't vary by feature size or shape.
- **Motion** → vibration (ringing/ghosting — aesthetic, mechanical/tuning issue, not
  derivable from geometry, out of scope) and instability (a tall/thin section wobbling under
  disturbance — structural).
- **Extrusion** → overhang (driven by nozzle pressure/filament momentum against the previous
  layer — this also happens on upside-down/non-gravity print orientations, so it is *not* a
  gravity effect) and stringing (parked — a post-processing concern, not pursued in this
  pass).

"Limited tolerance" (closely-spaced walls, small features) turned out not to be a nozzle/
extrusion resolution limit — slicers with variable line width handle sub-nozzle-width walls
fine. Two independent, non-redundant mechanisms drive it instead:

- **Reheat** (loop-level) — a small enclosed loop has a short total perimeter, so the nozzle
  finishes the loop and returns to the same region before it has cooled. This is a property of
  the *whole loop* (perimeter relative to print speed), not of any single edge or corner —
  unlike shrinkage, its severity varies per-feature, so no global compensation factor can
  correct it; Analysis has to flag it directly.
- **Cornering motion lag** (corner-level) — the nozzle can't change direction instantaneously,
  so it decelerates into a sharp corner and accelerates out. Extrusion rate lags the velocity
  change (this is what pressure/linear advance firmware features compensate for, imperfectly),
  causing local over-extrusion concentrated at the corner — independent of loop size.

These compound at a sharp corner on a small loop, but either can occur alone: a large loop with
one sharp corner gets a small, localized defect; a small loop with no sharp corners (e.g.
filleted) can still be at reheat risk from loop size alone.

This also means the existing **Sharp Corner** detector (3D dihedral angle, orientation-
independent) is measuring the wrong quantity for this purpose: printers print in layers, so
what matters is the *in-plane, per-layer* turning angle, which is orientation-dependent by
construction. A sharp 3D edge oriented to run mostly horizontal barely shows up as an in-plane
turn at all. Sharp Corner as a standalone orientation-independent check is retired; the angle
term it provided is folded into the per-layer corner score below.

## Final detector list — all slicing-based

1. **Overhang** — per-layer polygon boolean difference (`currentLayer − previousLayer`)
   gives the actual unsupported region directly. No point-correspondence between layer
   boundaries is needed (which would break under topology changes — a hole opening, a loop
   splitting/merging), since boolean difference is well-defined regardless of topology.
   Threshold metric: **reach distance**, not area — the perpendicular distance from the
   supported boundary to the diff region's leading (most unsupported) edge. Area alone is the
   wrong primary signal: a long thin sliver of overhang (small area, far reach) is the case that
   actually sags, while a short-but-wide overhang strip can have much more area yet barely droop,
   because sag is governed by how far unsupported filament has to bridge before regaining
   support underneath, not by total unsupported area. Area can stay as a secondary signal.

2. **Not enough space** (reheat + cornering motion lag) — per layer, per correctly-separated
   loop:
   - **Loop-level baseline**: estimate single-pass revisit time as `loopPerimeter /
     nominalPrintSpeed` and compare against a cooling-time threshold (same idea as a slicer's
     "minimum layer time" setting — material needs roughly that long to solidify before the
     next pass lands on it). This is the primary reheat signal and applies to the whole loop,
     independent of corner sharpness.
   - **Corner-level multiplier**: walk the loop's boundary, score each corner by turning angle
     (sharper → more deceleration/dwell → more local over-extrusion). Score each edge by its
     length **relative to the loop's own average edge length**, not an absolute mm threshold —
     `edgeLengthScore = avgEdgeLength / edgeLength`. Direction matters here: the short edge must
     end up in the denominator, since a short edge should score *higher* risk (less distance for
     a neighboring corner's effect to decay) — the inverse ratio would backwards-favor long
     edges. A relative measure also generalizes across loops of different overall size/print
     speed, where a fixed absolute length threshold wouldn't: a 1mm edge is unremarkable in a
     loop averaging 0.5mm edges, but anomalous in a loop averaging 10mm edges.
   - Combined: `risk = loopReheatRisk(loopPerimeter, nominalPrintSpeed) * (1 +
     cornerDwellBonus(cornerScoreA, cornerScoreB, edgeLengthScore))`. Flag edges above a
     threshold.
   - **Mapping the flag to a face**: each segment/corner of a per-layer loop comes from
     intersecting one specific solid face with the slicing plane, so it can be tagged with its
     originating `TopoDS_Face` at slice time, for free — no separate face-matching pass needed.
     The flagged sub-boundary becomes a clip region attached to that face's `Issue` (the
     `TopoDS_Face`-value successor to `FaceFlaw.clipBoundary`, as Small Feature did today) —
     this replaces flagging an entire face/loop wholesale.
   - The cooling-time threshold and the relative weight of the corner multiplier are not yet
     calibrated — pending print experiments (isolating loop size and corner sharpness
     independently) before this is trusted as more than a rough heuristic.

3. **Instability** (toppling) — per layer, per loop: compute `minWidth` via convex hull +
   rotating calipers (minimum extent measured perpendicular to any hull edge — the minimum
   width of a convex polygon is always achieved perpendicular to one of its hull edges, so
   only as many directions as the hull has edges need checking). This is the same principle
   as Euler buckling — a column yields along its weakest axis, not an average — and replaces
   the isotropic `2A/P`, which couldn't tell a stable panel from an unstable rod. Reuse the
   existing run-detection + height/width-ratio logic from Thin Section, just fed `minWidth`
   instead of `2A/P`.

4. **Layer difference** (warping) — the same polygon-difference primitive as Overhang, scored
   on raw area-delta magnitude between adjacent layers rather than a directional/unsupported
   offset. Shares the computation, different threshold on the output.
   - **Mapping the flag**: unlike Not Enough Space, this isn't a single-face defect — warping
     runs vertically through the part's height, so flagged diff regions are chained across
     consecutive layers into a continuous vertical ribbon rather than mapped back onto one
     originating face. This is exactly what `BridgeSurface` (`AnalysisTypes.hpp`) already
     represents: a closed polygon for a vertical connecting surface. Needs the same rework as
     `FaceFlaw` — `AnalysisResults.bridgeSurfaces` is currently keyed by raw `const Solid *`.

**Sharp Corner** and **Small Feature**/**Thin Section** as standalone detectors are retired,
superseded by 2 and 3 above. **Stringing** stays parked (post-processing concern, deprioritized,
not part of this pass) — existing code can remain dead or be deleted later.

## Architectural consequence

Every remaining detector operates on the same per-layer sliced cross-section. There is no
detector left that isn't slicing-based, so `IFaceAnalysis`/`IEdgeAnalysis` as separate
interfaces are no longer needed. Slicing should happen exactly once per solid, with all
detectors consuming that one shared set of per-layer polygons — instead of today, where Small
Feature, Thin Section, and Stringing each independently call `Slice::Range` on the same solid.

## Prerequisite fix

None of the above per-loop metrics (minWidth, corner/edge walk, polygon difference) can be
implemented correctly until loop separation is fixed: `ChainOuter` must detect multiple
disjoint closed loops per layer instead of greedily stitching every segment into one polygon
(e.g. via union-find on shared endpoints, the same technique `Stringing.cpp` already uses for
counting loops).

## Explicitly out of scope

- **Loose faces** (faces not part of a solid) — not analyzed. They have no enclosed volume to
  slice meaningfully; treated as an import-tool concern (sew into a solid), not a
  manufacturability flaw.
- **Vibration/ringing** — mechanical/tuning issue, not derivable from geometry alone.
- **Stringing** — parked; revisit only as a post-processing concern if ever.
- **Bed-slinger-specific anisotropic disturbance direction / auto-orient** — future work, not
  in this pass. Instability assumes roughly isotropic disturbance forces.
