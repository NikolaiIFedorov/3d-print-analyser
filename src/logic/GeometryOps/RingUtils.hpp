#pragma once

#include <glm/glm.hpp>

#include <TopoDS_Edge.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pnt.hxx>

#include <vector>

// Tool-agnostic geometry primitives over polyline "rings" (closed loops of 3D points) and the OCCT
// wires/edges they're sampled from. Originally built for the Structure tool's offset/trim/strut
// pipeline, but these operate purely on glm/TopoDS types — no Structure-specific (BakeParams, Face*)
// coupling — so any future geometry-editing tool can reuse them directly.
namespace GeometryOps
{

/// Upward world normal, used as the default basis for footprints built/measured in the world-XY plane.
constexpr glm::dvec3 kWorldUpNormal{0.0, 0.0, 1.0};

glm::dvec3 ProjectPointToXyPlane(const glm::dvec3 &p, double zBottom);

/// Builds an orthonormal (u, v) basis perpendicular to `normalHint`, for projecting 3D rings to 2D.
void BuildPlanarBasis(const glm::dvec3 &normalHint, glm::dvec3 &uOut, glm::dvec3 &vOut);

/// Signed polygon area of `ring` projected onto the plane perpendicular to `normalHint`.
double SignedArea2D(const std::vector<glm::dvec3> &ring, const glm::dvec3 &normalHint);

/// Splits `rings` into the single largest-area outer ring (CCW) and the rest as holes (CW).
void ClassifyOuterAndHoles(const std::vector<std::vector<glm::dvec3>> &rings, const glm::dvec3 &normalHint,
                           std::vector<glm::dvec3> &outerOut,
                           std::vector<std::vector<glm::dvec3>> &holesOut);

void AppendRingPoint(std::vector<glm::dvec3> &ring, const gp_Pnt &p);

/// Samples `edge` into `points` at `chordTolMm` chord deflection (lines: just the two endpoints).
void CollectPointsFromEdge(const TopoDS_Edge &edge, double chordTolMm, std::vector<glm::dvec3> &points);

void RemoveConsecutiveDuplicateRingPoints(std::vector<glm::dvec3> &points, double tol);

glm::dvec2 RingCentroidXy(const std::vector<glm::dvec3> &ring);

double RingSpanXyMm(const std::vector<glm::dvec3> &ring);

double RingMaxSpanMm(const std::vector<glm::dvec3> &ring);

/// Samples `wire`'s edges in wire order at `chordTolMm` chord deflection into one continuous ring,
/// so segment i->i+1 never jumps across an edge join.
std::vector<glm::dvec3> ExtractRingPointsInWireOrder(const TopoDS_Wire &wire, double chordTolMm);

bool PointInRingPlanar(glm::dvec2 p, const std::vector<glm::dvec3> &ring, const glm::dvec3 &u,
                       const glm::dvec3 &v);

double RingPerimeterMm(const std::vector<glm::dvec3> &ring);

/// 1.0 = circle; lower = more elongated.
double RingRoundness(const std::vector<glm::dvec3> &ring, const glm::dvec3 &basisNormal);

/// Inner ring index (1..n-1) whose interior contains p; -1 if none.
int FindInnerRingContainingPoint(const glm::dvec2 &p, const std::vector<std::vector<glm::dvec3>> &rings,
                                 const glm::dvec3 &basisNormal);

bool InsideFootprintPlanar(glm::dvec2 p, const std::vector<glm::dvec3> &outerRing,
                           const std::vector<std::vector<glm::dvec3>> &holeRings, const glm::dvec3 &u,
                           const glm::dvec3 &v);

} // namespace GeometryOps
