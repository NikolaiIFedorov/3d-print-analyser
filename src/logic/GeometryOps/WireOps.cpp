#include "GeometryOps/WireOps.hpp"

#include "GeometryOps/RingUtils.hpp"
#include "GeometryOps/Topology.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <GeomAbs_CurveType.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>

namespace GeometryOps
{

TopoDS_Wire MakeFlatWireFromRing(const std::vector<glm::dvec3> &ring, double zBottom)
{
    if (ring.size() < 3)
        return TopoDS_Wire();

    BRepBuilderAPI_MakePolygon poly;
    for (const glm::dvec3 &p : ring)
        poly.Add(gp_Pnt(p.x, p.y, zBottom));
    poly.Close();
    if (!poly.IsDone())
        return TopoDS_Wire();
    return poly.Wire();
}

bool TryGetPlanarCircleFromWire(const TopoDS_Wire &wire, PlanarCircle &out)
{
    TopoDS_Edge edge;
    int edgeCount = 0;
    for (TopExp_Explorer ex(wire, TopAbs_EDGE); ex.More(); ex.Next())
    {
        if (edgeCount > 0)
            return false;
        edge = TopoDS::Edge(ex.Current());
        ++edgeCount;
    }
    if (edgeCount != 1)
        return false;

    BRepAdaptor_Curve curve(edge);
    if (curve.GetType() != GeomAbs_Circle)
        return false;

    const gp_Circ gc = curve.Circle();
    const gp_Pnt loc = gc.Location();
    out.center = glm::dvec3(loc.X(), loc.Y(), loc.Z());
    out.radius = gc.Radius();
    const gp_Dir n = gc.Axis().Direction();
    out.normal = glm::dvec3(n.X(), n.Y(), n.Z());
    const gp_Dir x = gc.XAxis().Direction();
    out.u = glm::dvec3(x.X(), x.Y(), x.Z());
    return out.radius > 1e-9;
}

TopoDS_Wire MakePlanarCircleWire(const PlanarCircle &circle, double radiusMm,
                                 const std::vector<glm::dvec3> &referenceRing,
                                 const glm::dvec3 &basisNormal, double chordTolMm)
{
    if (radiusMm <= 1e-9)
        return TopoDS_Wire();

    const gp_Pnt center(circle.center.x, circle.center.y, circle.center.z);
    const gp_Dir normal(circle.normal.x, circle.normal.y, circle.normal.z);
    const gp_Dir xDir(circle.u.x, circle.u.y, circle.u.z);
    const gp_Ax2 ax2(center, normal, xDir);
    const gp_Circ gc(ax2, radiusMm);

    BRepBuilderAPI_MakeEdge mkEdge(gc);
    if (!mkEdge.IsDone())
        return TopoDS_Wire();
    TopoDS_Edge edge = mkEdge.Edge();
    BRepBuilderAPI_MakeWire mkWire(edge);
    if (!mkWire.IsDone())
        return TopoDS_Wire();
    TopoDS_Wire wire = mkWire.Wire();

    if (referenceRing.size() >= 3)
    {
        const std::vector<glm::dvec3> outRing = DiscretizeWireToRing3D(wire, chordTolMm);
        if (outRing.size() >= 3)
        {
            const double refSigned = SignedArea2D(referenceRing, basisNormal);
            const double outSigned = SignedArea2D(outRing, basisNormal);
            if (refSigned * outSigned < 0.0)
                wire.Reverse();
        }
    }
    return wire;
}

// Detects whether a discretized ring is (within chord-deflection tolerance) a uniformly sampled
// circle — i.e. it came from a circular hole/outline that survived offsetting untouched by any
// downstream notching/filleting. `GCPnts_QuasiUniformDeflection` guarantees every sample point
// lies within `chordTolMm` of the true curve, so a real sampled circle's points all land within
// that tolerance of one center/radius; anything reshaped downstream (fillet rounding, strut
// notches) won't pass this check and falls through to the polygon path untouched.
bool TryFitCircleFromFlatRing(const std::vector<glm::dvec3> &ring, double zPlane, double chordTolMm,
                              PlanarCircle &out)
{
    // Below this point count, a regular polygon (e.g. a small square notch) can look "circle-like"
    // within tolerance; real sampled circles at this pipeline's tessellation density produce far
    // more points than that, so this threshold is a cheap, effective discriminator.
    constexpr size_t kMinSamplesForCircle = 16;
    if (ring.size() < kMinSamplesForCircle)
        return false;

    glm::dvec3 centroid{0.0};
    for (const glm::dvec3 &p : ring)
        centroid += p;
    centroid /= static_cast<double>(ring.size());

    double sumRadius = 0.0;
    for (const glm::dvec3 &p : ring)
        sumRadius += glm::length(glm::dvec2(p.x - centroid.x, p.y - centroid.y));
    const double avgRadius = sumRadius / static_cast<double>(ring.size());
    if (avgRadius < 1e-6)
        return false;

    const double radialTol = std::max(chordTolMm * 2.0, 1e-5);
    for (const glm::dvec3 &p : ring)
    {
        if (std::abs(p.z - zPlane) > radialTol)
            return false;
        const double radius = glm::length(glm::dvec2(p.x - centroid.x, p.y - centroid.y));
        if (std::abs(radius - avgRadius) > radialTol)
            return false;
    }

    out.center = glm::dvec3(centroid.x, centroid.y, zPlane);
    out.normal = glm::dvec3(0.0, 0.0, 1.0);
    const glm::dvec2 toFirst(ring.front().x - centroid.x, ring.front().y - centroid.y);
    const double uLen = glm::length(toFirst);
    out.u = uLen > 1e-9 ? glm::dvec3(toFirst.x / uLen, toFirst.y / uLen, 0.0) : glm::dvec3(1.0, 0.0, 0.0);
    out.radius = avgRadius;
    return true;
}

bool TryFitPlanarCircleFromRing(const std::vector<glm::dvec3> &ring, const glm::dvec3 &basisNormal,
                                double chordTolMm, PlanarCircle &out)
{
    constexpr size_t kMinSamplesForCircle = 16;
    if (ring.size() < kMinSamplesForCircle)
        return false;

    glm::dvec3 u;
    glm::dvec3 v;
    BuildPlanarBasis(basisNormal, u, v);
    glm::dvec3 n = basisNormal;
    const double nLen = glm::length(n);
    if (nLen > 1e-12)
        n /= nLen;
    else
        n = glm::dvec3(0.0, 0.0, 1.0);

    glm::dvec3 centroid3D{0.0};
    for (const glm::dvec3 &p : ring)
        centroid3D += p;
    centroid3D /= static_cast<double>(ring.size());
    const glm::dvec2 centroid2D(glm::dot(centroid3D, u), glm::dot(centroid3D, v));
    const double planeOffset = glm::dot(centroid3D, n);

    double sumRadius = 0.0;
    std::vector<glm::dvec2> local2D;
    local2D.reserve(ring.size());
    for (const glm::dvec3 &p : ring)
    {
        const glm::dvec2 uv(glm::dot(p, u) - centroid2D.x, glm::dot(p, v) - centroid2D.y);
        local2D.push_back(uv);
        sumRadius += glm::length(uv);
    }
    const double avgRadius = sumRadius / static_cast<double>(ring.size());
    if (avgRadius < 1e-6)
        return false;

    const double radialTol = std::max(chordTolMm * 2.0, 1e-5);
    for (size_t i = 0; i < ring.size(); ++i)
    {
        if (std::abs(glm::dot(ring[i], n) - planeOffset) > radialTol)
            return false;
        const double radius = glm::length(local2D[i]);
        if (std::abs(radius - avgRadius) > radialTol)
            return false;
    }

    out.center = centroid3D;
    out.normal = n;
    const glm::dvec2 toFirst = local2D.front();
    const double uLen = glm::length(toFirst);
    out.u = uLen > 1e-9 ? glm::normalize(u * toFirst.x + v * toFirst.y) : u;
    out.radius = avgRadius;
    return true;
}

// Builds a flat wire for `ring`, preferring an exact `gp_Circ` edge over a polygon when the ring
// is detectably a sampled circle (see `TryFitCircleFromFlatRing`). This is the Phase-1 fix for the
// over-tessellation blowup documented in project memory (carve TRAP #2): round holes/outlines —
// by far the most common curved feature in carved footprints — stop being flattened into
// ~145-segment polylines right before extrusion, so `BRepPrimAPI_MakePrism` produces a true
// cylinder wall instead of a many-faced prism. Falls back to the polygon path whenever detection
// fails or the exact wire fails to build/validate, so reshaped geometry (fillets, notches) and
// non-circular rings are completely unaffected — this can only ever reduce face count, never
// change which regions get cut.
TopoDS_Wire MakeFlatWireFromRingOrCircle(const std::vector<glm::dvec3> &ring, double zBottom, double chordTolMm)
{
    PlanarCircle circle;
    if (TryFitCircleFromFlatRing(ring, zBottom, chordTolMm, circle))
    {
        const TopoDS_Wire circleWire =
            MakePlanarCircleWire(circle, circle.radius, ring, kWorldUpNormal, chordTolMm);
        if (!circleWire.IsNull() &&
            ValidateOutlineShapeTopology(circleWire, "MakeFlatWireFromRingOrCircle[circle]"))
            return circleWire;
    }
    return MakeFlatWireFromRing(ring, zBottom);
}

TopoDS_Wire OffsetFlatWire(const TopoDS_Wire &wire, double offsetMm)
{
    if (wire.IsNull() || std::abs(offsetMm) <= 1e-6)
        return wire;

    BRepOffsetAPI_MakeOffset offsetMaker;
    offsetMaker.Init(GeomAbs_Arc);
    offsetMaker.AddWire(wire);
    offsetMaker.Perform(offsetMm);
    if (!offsetMaker.IsDone())
        return TopoDS_Wire();

    for (TopExp_Explorer ex(offsetMaker.Shape(), TopAbs_WIRE); ex.More(); ex.Next())
        return TopoDS::Wire(ex.Current());
    return TopoDS_Wire();
}

std::vector<glm::dvec3> DiscretizeWireToXyRing(const TopoDS_Wire &wire, double zBottom, double chordTolMm)
{
    std::vector<glm::dvec3> ring = ExtractRingPointsInWireOrder(wire, chordTolMm);
    for (glm::dvec3 &p : ring)
        p = ProjectPointToXyPlane(p, zBottom);
    RemoveConsecutiveDuplicateRingPoints(ring, 1e-6);
    return ring;
}

std::vector<glm::dvec3> DiscretizeWireToRing3D(const TopoDS_Wire &wire, double chordTolMm)
{
    std::vector<glm::dvec3> ring = ExtractRingPointsInWireOrder(wire, chordTolMm);
    RemoveConsecutiveDuplicateRingPoints(ring, 1e-6);
    return ring;
}

TopoDS_Wire OffsetWireWithFallback(const TopoDS_Wire &wire, double primaryOffsetMm, double fallbackOffsetMm)
{
    if (wire.IsNull())
        return TopoDS_Wire();
    TopoDS_Wire result = OffsetFlatWire(wire, primaryOffsetMm);
    if (!result.IsNull())
        return result;
    return OffsetFlatWire(wire, fallbackOffsetMm);
}

TopoDS_Wire OffsetOuterWireInward(const TopoDS_Wire &wire, double insetMm, const glm::dvec3 &basisNormal,
                                  double chordTolMm)
{
    if (wire.IsNull() || insetMm <= 1e-6)
        return wire;

    const std::vector<glm::dvec3> ringIn = DiscretizeWireToRing3D(wire, chordTolMm);
    if (ringIn.size() < 3)
        return TopoDS_Wire();
    const double areaIn = std::abs(SignedArea2D(ringIn, basisNormal));

    auto tryOffset = [&](double delta) -> TopoDS_Wire
    {
        const TopoDS_Wire result = OffsetFlatWire(wire, delta);
        if (result.IsNull())
            return TopoDS_Wire();
        const std::vector<glm::dvec3> ringOut = DiscretizeWireToRing3D(result, chordTolMm);
        if (ringOut.size() < 3)
            return TopoDS_Wire();
        const double areaOut = std::abs(SignedArea2D(ringOut, basisNormal));
        if (areaOut + 1e-6 < areaIn)
            return result;
        return TopoDS_Wire();
    };

    if (TopoDS_Wire inset = tryOffset(-insetMm); !inset.IsNull())
        return inset;
    return tryOffset(insetMm);
}

TopoDS_Wire OffsetHoleWireOutward(const TopoDS_Wire &wire, double insetMm, const glm::dvec3 &basisNormal,
                                  double chordTolMm)
{
    if (wire.IsNull() || insetMm <= 1e-6)
        return wire;

    const std::vector<glm::dvec3> ringIn = DiscretizeWireToRing3D(wire, chordTolMm);
    if (ringIn.size() < 3)
        return TopoDS_Wire();
    const double areaIn = std::abs(SignedArea2D(ringIn, basisNormal));

    PlanarCircle circle;
    bool hasOcctCircle = TryGetPlanarCircleFromWire(wire, circle);
    if (!hasOcctCircle)
        hasOcctCircle = TryFitPlanarCircleFromRing(ringIn, basisNormal, chordTolMm, circle);

    auto tryOffset = [&](double delta) -> TopoDS_Wire
    {
        if (hasOcctCircle)
        {
            const double newRadius = circle.radius + delta;
            if (newRadius <= 1e-6)
                return TopoDS_Wire();
            const TopoDS_Wire result = MakePlanarCircleWire(circle, newRadius, ringIn, basisNormal, chordTolMm);
            if (result.IsNull())
                return TopoDS_Wire();
            const std::vector<glm::dvec3> ringOut = DiscretizeWireToRing3D(result, chordTolMm);
            if (ringOut.size() < 3)
                return TopoDS_Wire();
            const double areaOut = std::abs(SignedArea2D(ringOut, basisNormal));
            if (areaOut > areaIn + 1e-6)
                return result;
            return TopoDS_Wire();
        }

        const TopoDS_Wire result = OffsetFlatWire(wire, delta);
        if (result.IsNull())
            return TopoDS_Wire();
        const std::vector<glm::dvec3> ringOut = DiscretizeWireToRing3D(result, chordTolMm);
        if (ringOut.size() < 3)
            return TopoDS_Wire();
        const double areaOut = std::abs(SignedArea2D(ringOut, basisNormal));
        if (areaOut > areaIn + 1e-6)
            return result;
        return TopoDS_Wire();
    };

    if (TopoDS_Wire outset = tryOffset(insetMm); !outset.IsNull())
        return outset;
    return tryOffset(-insetMm);
}

} // namespace GeometryOps
