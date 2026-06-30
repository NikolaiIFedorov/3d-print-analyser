#pragma once

#include <glm/glm.hpp>

#include <TopoDS_Wire.hxx>

#include <vector>

// Flat (single-plane) wire construction, offsetting, and circle-detection primitives, built for the
// Structure tool's offset/inset pipeline but generic over TopoDS_Wire/glm types.
namespace GeometryOps
{

TopoDS_Wire MakeFlatWireFromRing(const std::vector<glm::dvec3> &ring, double zBottom);

struct PlanarCircle
{
    glm::dvec3 center{0.0};
    glm::dvec3 normal{0.0, 0.0, 1.0};
    glm::dvec3 u{1.0, 0.0, 0.0};
    double radius = 0.0;
};

/// True if `wire` is a single OCCT circle edge; fills `out` with its geometry.
bool TryGetPlanarCircleFromWire(const TopoDS_Wire &wire, PlanarCircle &out);

/// Builds an exact circular wire at `radiusMm`, oriented to match `referenceRing`'s winding.
TopoDS_Wire MakePlanarCircleWire(const PlanarCircle &circle, double radiusMm,
                                 const std::vector<glm::dvec3> &referenceRing,
                                 const glm::dvec3 &basisNormal, double chordTolMm);

/// Detects whether a discretized ring is (within chord-deflection tolerance) a uniformly sampled
/// circle — i.e. it came from a circular hole/outline that survived offsetting untouched by any
/// downstream notching/filleting.
bool TryFitCircleFromFlatRing(const std::vector<glm::dvec3> &ring, double zPlane, double chordTolMm,
                              PlanarCircle &out);

/// Same numeric detection as `TryFitCircleFromFlatRing`, generalized to a ring lying on any plane
/// (not just z=const) given via `basisNormal`. Catches circles whose underlying OCCT curve isn't
/// tagged `GeomAbs_Circle` — e.g. an ellipse that was pre-distorted so a later affine flattening
/// transform (`BRepBuilderAPI_GTransform`) maps it onto an exact circle: the transform doesn't
/// retag the curve type, so `TryGetPlanarCircleFromWire` misses it even though the ring is, in
/// fact, geometrically circular.
bool TryFitPlanarCircleFromRing(const std::vector<glm::dvec3> &ring, const glm::dvec3 &basisNormal,
                                double chordTolMm, PlanarCircle &out);

/// Builds a flat wire for `ring`, preferring an exact `gp_Circ` edge over a polygon when the ring
/// is detectably a sampled circle (see `TryFitCircleFromFlatRing`). Falls back to the polygon path
/// whenever detection fails or the exact wire fails to build/validate.
TopoDS_Wire MakeFlatWireFromRingOrCircle(const std::vector<glm::dvec3> &ring, double zBottom, double chordTolMm);

TopoDS_Wire OffsetFlatWire(const TopoDS_Wire &wire, double offsetMm);

std::vector<glm::dvec3> DiscretizeWireToXyRing(const TopoDS_Wire &wire, double zBottom, double chordTolMm);

std::vector<glm::dvec3> DiscretizeWireToRing3D(const TopoDS_Wire &wire, double chordTolMm);

TopoDS_Wire OffsetWireWithFallback(const TopoDS_Wire &wire, double primaryOffsetMm, double fallbackOffsetMm);

/// Offsets `wire` inward by `insetMm`, verifying the result's area actually shrank (guards against
/// OCCT returning the wrong-direction offset branch).
TopoDS_Wire OffsetOuterWireInward(const TopoDS_Wire &wire, double insetMm, const glm::dvec3 &basisNormal,
                                  double chordTolMm);

/// Offsets `wire` outward by `insetMm` (exact circle growth when `wire` is a circle), verifying the
/// result's area actually grew.
TopoDS_Wire OffsetHoleWireOutward(const TopoDS_Wire &wire, double insetMm, const glm::dvec3 &basisNormal,
                                  double chordTolMm);

} // namespace GeometryOps
