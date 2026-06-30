#pragma once

#include <glm/glm.hpp>

#include <vector>

// 2D polygon (ring) boolean difference, implemented by routing through OCCT face booleans
// rather than a hand-rolled clipper — no 2D polygon-clipping library exists in this codebase,
// and a general hand-rolled clipper carries real correctness risk (shared edges, near-degenerate
// loops) with no existing precedent to lean on. Reuses GeometryOps::WireOps' proven flat-wire
// construction/discretization.
namespace GeometryOps
{

/// A single ring (closed loop) plus whether it's a hole within its layer's other rings.
/// Mirrors `SliceLoop` (src/logic/Analysis/AnalysisTypes.hpp) structurally but is defined
/// independently here so GeometryOps stays decoupled from Analysis-specific types.
struct ClassifiedRing
{
    std::vector<glm::dvec3> ring;
    bool isHole = false;
};

/// `a - b`: the region covered by `a`'s rings (outer rings minus their holes) that is not also
/// covered by `b`'s rings. Both inputs may contain multiple disjoint outer islands, each with
/// any number of holes — island/hole pairing doesn't need to be known by the caller, since
/// fusing all outers and subtracting the fused union of all holes is equivalent to doing it
/// island-by-island. Returns the result's rings (outer + holes, classified the same way),
/// discretized back to flat XY rings at `zBottom`.
std::vector<ClassifiedRing> RingDifference(const std::vector<ClassifiedRing> &a,
                                           const std::vector<ClassifiedRing> &b, double zBottom,
                                           double chordTolMm);

/// For each ring in `b`, returns its intersection with the fused outer shape built from `a`.
/// Rings fully outside `a` are omitted; rings partially extending outside are clipped to the
/// portion inside `a`. This clips hole circles that extend past the plate boundary at high Z
/// layers, without needing inner wires from BRepAlgoAPI_Cut (which OCCT doesn't provide for
/// flat polygon-face booleans).
std::vector<std::vector<glm::dvec3>> ClipHolesToOuter(const std::vector<ClassifiedRing> &a,
                                                       const std::vector<ClassifiedRing> &b,
                                                       double zBottom, double chordTolMm);

} // namespace GeometryOps
