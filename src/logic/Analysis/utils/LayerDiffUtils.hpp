#pragma once

#include "Analysis/AnalysisTypes.hpp"
#include "GeometryOps/RingBoolean.hpp"

#include <unordered_set>
#include <vector>

/// Small shared helpers used by both Overhang and Layer Difference, which are the same
/// underlying primitive (per-layer-pair polygon boolean difference) with different thresholds
/// applied to the result.
namespace LayerDiffUtils
{

/// Converts loop-extraction output to the form `GeometryOps::RingDifference` expects.
std::vector<GeometryOps::ClassifiedRing> ToClassifiedRings(const std::vector<SliceLoop> &loops);

/// Maps an arbitrary point (e.g. a vertex on a freshly computed OCCT boolean result, which has
/// no inherent face provenance of its own) to whichever original loop edge in `loops` it's
/// closest to in XY, and returns that edge's face. Used to attribute a diff result's region back
/// to the face(s) it belongs to for `FaceFlaw`.
const Face *NearestEdgeFace(const glm::dvec3 &p, const std::vector<SliceLoop> &loops);

/// Returns the set of faces attributed to a diff ring. Matches diff ring edges against original
/// loop edges (edgeFaces tags), counts attributions per face, and returns the dominant face —
/// but only if that face's own normal passes the overhang angle gate (normal.z < -cos(overhangAngleDeg)).
/// Faces that are not themselves overhanging (vertical walls, upward-facing floors) are excluded
/// so that a side wall that merely borders the overhang perimeter isn't falsely highlighted.
/// Returns empty if no eligible face is found. Falls back to NearestEdgeFace for unmatched edges.
std::unordered_set<const Face *> FacesForDiffRing(const std::vector<glm::dvec3> &diffRing,
                                                   const std::vector<SliceLoop> &loops,
                                                   double tolerance = 1e-3,
                                                   double overhangAngleDeg = 45.0);

/// Cheap (pure-glm, no OCCT) pre-filter: true if `a` and `b` have the same loop count and each
/// loop in `a` has a matching loop in `b` (by nearest centroid) with near-identical area and
/// centroid. The overwhelming majority of adjacent layer pairs in a real model are unchanged
/// vertical wall cross-sections — running a full OCCT boolean difference on every layer pair
/// regardless was the dominant cost in profiling (~100+ layers x multiple OCCT calls per pair).
/// Skipping the OCCT path whenever this cheap check confirms nothing meaningful changed makes the
/// common case nearly free, while still falling through to the exact boolean diff whenever a
/// layer pair might actually differ.
bool LayersAreEffectivelyIdentical(const std::vector<SliceLoop> &a, const std::vector<SliceLoop> &b,
                                   double areaEpsMm2 = 1e-4, double centroidEpsMm = 1e-3);

/// True if any loop in `loops` is already thinner than `minWidthEpsMm` (e.g. the very tip of a
/// tapering wedge, approaching a degenerate sliver as width -> 0). OCCT's boolean algorithms can
/// take pathologically long on near-degenerate input wires/faces; there's also nothing
/// meaningful to compute a boolean difference against once a cross-section has already shrunk
/// to a sliver below printable size (a real flaw there, but one Instability/Not Enough Space
/// already cover via their own — cheap, non-OCCT — width checks).
bool AnyLoopIsDegenerate(const std::vector<SliceLoop> &loops, double minWidthEpsMm = 0.05);

} // namespace LayerDiffUtils
