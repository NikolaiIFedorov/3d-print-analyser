#pragma once

#include <glm/glm.hpp>

#include <vector>

// Pure-glm 2D convex hull and minimum-width primitives, generic over any ring/point set
// projected onto a plane via `normalHint` (consistent with RingUtils' planar-basis convention).
// No OCCT dependency.
namespace GeometryOps
{

/// Convex hull of `points` projected onto the plane perpendicular to `normalHint`, returned as
/// the corresponding original (un-projected) 3D points in CCW order, no duplicate closing point.
/// Collinear points on the hull boundary are dropped (degenerate input — e.g. all points
/// collinear — returns the two extreme points only).
std::vector<glm::dvec3> ConvexHull2D(const std::vector<glm::dvec3> &points, const glm::dvec3 &normalHint);

/// Minimum width of `points`' convex hull: the smallest extent measured perpendicular to any
/// hull edge. The minimum width of a convex polygon is always achieved perpendicular to one of
/// its edges, so only as many directions as the hull has edges need checking (rotating
/// calipers). This is the same principle behind Euler buckling — a structure's resistance is
/// governed by its weakest-direction extent, not an isotropic average like `2A/P`. Returns 0.0
/// for degenerate (fewer than 3 non-collinear points) input.
double MinWidth2D(const std::vector<glm::dvec3> &points, const glm::dvec3 &normalHint);

} // namespace GeometryOps
