#include "Structure/StructureTriangulation.hpp"
#include "Geometry/Face.hpp"
#include "Geometry/OrientedEdge.hpp"
#include "Geometry/Edge.hpp"
#include "Geometry/Point.hpp"
#include "Geometry/Surface.hpp"
#include "utils/log.hpp"

#include "GeometryOps/Fillet2D.hpp"
#include "GeometryOps/PlaneProjection.hpp"
#include "GeometryOps/RingUtils.hpp"
#include "GeometryOps/Topology.hpp"
#include "GeometryOps/WireOps.hpp"

#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <ChFi2d_FilletAPI.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <Standard_Failure.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <gp_GTrsf.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <GProp_GProps.hxx>
#include <Geom_Surface.hxx>
#include <ShapeFix_Face.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_CompCurve.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopExp.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>
#include <TColgp_Array1OfPnt.hxx>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <unordered_map>

namespace StructureTriangulation
{

using namespace GeometryOps;

std::vector<std::vector<glm::dvec3>> BuildFacePlaneOffsetPreviewRings(const Face *face,
                                                                      const BakeParams &params);

namespace
{

struct CacheKey
{
    const Face *face;
    double insetMm;
    double chordTolMm;
    double minFeatureMm;
    bool weightAwareStrutFilter;
    double wallThicknessMm;
    bool operator==(const CacheKey &other) const noexcept
    {
        return face == other.face && insetMm == other.insetMm && chordTolMm == other.chordTolMm &&
               minFeatureMm == other.minFeatureMm &&
               weightAwareStrutFilter == other.weightAwareStrutFilter &&
               wallThicknessMm == other.wallThicknessMm;
    }
};

struct CacheKeyHash
{
    std::size_t operator()(const CacheKey &k) const noexcept
    {
        std::size_t h = std::hash<const Face *>{}(k.face);
        h ^= std::hash<long long>{}(static_cast<long long>(k.insetMm * 1e6)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<long long>{}(static_cast<long long>(k.chordTolMm * 1e6)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<long long>{}(static_cast<long long>(k.minFeatureMm * 1e6)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.weightAwareStrutFilter) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<long long>{}(static_cast<long long>(k.wallThicknessMm * 1e6)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

std::unordered_map<CacheKey, std::vector<std::pair<glm::vec3, glm::vec3>>, CacheKeyHash> &LineBakeCache()
{
    static std::unordered_map<CacheKey, std::vector<std::pair<glm::vec3, glm::vec3>>, CacheKeyHash> cache;
    return cache;
}

// The Z plane the curve-native footprint pipeline projects/builds source geometry onto before
// extruding back to world space — arbitrary, since only relative Z within the footprint matters.
constexpr double kXyPlaneZ = 0.0;


bool StructureDebugEnabled()
{
    const char *debugEnv = std::getenv("CAD_DEBUG_STRUCTURE");
    return debugEnv != nullptr && debugEnv[0] != '\0' && !(debugEnv[0] == '0' && debugEnv[1] == '\0');
}

bool StructureDebugStepCutMode()
{
    return std::getenv("CAD_DEBUG_STRUCTURE_MAX_CUTS") != nullptr ||
           std::getenv("CAD_DEBUG_STRUCTURE_CUT_ONLY") != nullptr;
}

int StructureDebugEnvInt(const char *name, int unsetValue)
{
    const char *value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
        return unsetValue;
    return std::atoi(value);
}

TopoDS_Face BuildFallbackFaceFromLoops(const Face *face)
{
    if (face == nullptr || face->loops.empty())
        return TopoDS_Face();

    std::unique_ptr<BRepBuilderAPI_MakeFace> mkFace;
    for (const auto &loop : face->loops)
    {
        BRepBuilderAPI_MakePolygon poly;
        for (const OrientedEdge &oe : loop)
        {
            if (oe.GetStart() != nullptr)
            {
                glm::dvec3 p = oe.GetStartPosition();
                poly.Add(gp_Pnt(p.x, p.y, p.z));
            }
        }
        poly.Close();
        if (poly.IsDone())
        {
            if (!mkFace)
                mkFace = std::make_unique<BRepBuilderAPI_MakeFace>(poly.Wire());
            else
                mkFace->Add(poly.Wire());
        }
    }

    if (!mkFace || !mkFace->IsDone())
        return TopoDS_Face();
    return mkFace->Face();
}

TopoDS_Face ResolvePlanarOcctFace(const Face *face)
{
    if (face != nullptr && !face->occtFace.IsNull())
        return face->occtFace;
    return BuildFallbackFaceFromLoops(face);
}


bool PointInRingXy(glm::dvec2 p, const std::vector<glm::dvec3> &ring)
{
    if (ring.size() < 3)
        return false;
    bool inside = false;
    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
    {
        const glm::dvec2 a(ring[i].x, ring[i].y);
        const glm::dvec2 b(ring[j].x, ring[j].y);
        const bool intersects = ((a.y > p.y) != (b.y > p.y)) &&
                                (p.x < (b.x - a.x) * (p.y - a.y) / ((b.y - a.y) + 1e-30) + a.x);
        if (intersects)
            inside = !inside;
    }
    return inside;
}

struct CarveRingGroup
{
    std::vector<glm::dvec3> outer;
    std::vector<std::vector<glm::dvec3>> holes;
};

std::vector<CarveRingGroup> PartitionRingsIntoCarveGroups(const std::vector<std::vector<glm::dvec3>> &rings,
                                                          const glm::dvec3 &basisNormal)
{
    std::vector<CarveRingGroup> groups;
    if (rings.empty())
        return groups;

    struct RingInfo
    {
        size_t index;
        double absArea;
        glm::dvec2 centroid;
    };
    std::vector<RingInfo> info;
    info.reserve(rings.size());
    for (size_t i = 0; i < rings.size(); ++i)
    {
        if (rings[i].size() < 3)
            continue;
        info.push_back({i, std::abs(SignedArea2D(rings[i], basisNormal)), RingCentroidXy(rings[i])});
    }
    if (info.empty())
        return groups;

    std::sort(info.begin(), info.end(),
              [](const RingInfo &a, const RingInfo &b) { return a.absArea > b.absArea; });

    std::vector<int> parent(info.size(), -1);
    for (size_t i = 0; i < info.size(); ++i)
    {
        for (size_t j = 0; j < i; ++j)
        {
            if (PointInRingXy(info[i].centroid, rings[info[j].index]))
            {
                parent[i] = static_cast<int>(j);
                break;
            }
        }
    }

    for (size_t i = 0; i < info.size(); ++i)
    {
        if (parent[i] >= 0)
            continue;
        CarveRingGroup group;
        group.outer = rings[info[i].index];
        if (SignedArea2D(group.outer, basisNormal) < 0.0)
            std::reverse(group.outer.begin(), group.outer.end());

        for (size_t k = 0; k < info.size(); ++k)
        {
            if (parent[k] != static_cast<int>(i))
                continue;
            std::vector<glm::dvec3> hole = rings[info[k].index];
            if (SignedArea2D(hole, basisNormal) > 0.0)
                std::reverse(hole.begin(), hole.end());
            group.holes.push_back(std::move(hole));
        }
        groups.push_back(std::move(group));
    }
    return groups;
}


std::vector<std::vector<glm::dvec3>> CollectRingsFromShape(const TopoDS_Shape &shape, double chordTolMm,
                                                           double zPlane, double minSpanMm)
{
    std::vector<std::vector<glm::dvec3>> rings;
    if (shape.IsNull())
        return rings;

    auto tryAddRing = [&](const TopoDS_Wire &wire)
    {
        std::vector<glm::dvec3> ring = ExtractRingPointsInWireOrder(wire, chordTolMm);
        if (ring.size() < 3 || RingSpanXyMm(ring) < minSpanMm)
            return;
        for (glm::dvec3 &p : ring)
            p = ProjectPointToXyPlane(p, zPlane);
        rings.push_back(std::move(ring));
    };

    if (shape.ShapeType() == TopAbs_FACE)
    {
        const TopoDS_Face face = TopoDS::Face(shape);
        for (TopExp_Explorer exWire(face, TopAbs_WIRE); exWire.More(); exWire.Next())
            tryAddRing(TopoDS::Wire(exWire.Current()));
        return rings;
    }

    for (TopExp_Explorer exWire(shape, TopAbs_WIRE); exWire.More(); exWire.Next())
        tryAddRing(TopoDS::Wire(exWire.Current()));

    if (rings.empty())
    {
        for (TopExp_Explorer exFace(shape, TopAbs_FACE); exFace.More(); exFace.Next())
        {
            const TopoDS_Face f = TopoDS::Face(exFace.Current());
            for (TopExp_Explorer exWire(f, TopAbs_WIRE); exWire.More(); exWire.Next())
                tryAddRing(TopoDS::Wire(exWire.Current()));
        }
    }
    return rings;
}

std::vector<std::vector<glm::dvec3>> CollectRingsFromFaceLoops(const Face *face, double zPlane, double chordTolMm,
                                                               double minSpanMm)
{
    std::vector<std::vector<glm::dvec3>> rings;
    if (face == nullptr)
        return rings;
    for (const auto &loop : face->loops)
    {
        std::vector<glm::dvec3> ring;
        for (const OrientedEdge &oe : loop)
        {
            if (oe.edge == nullptr)
                continue;
            if (!oe.edge->occtEdge.IsNull())
            {
                TopoDS_Edge edge = oe.edge->occtEdge;
                if (oe.reversed)
                    edge.Reverse();
                CollectPointsFromEdge(edge, chordTolMm, ring);
            }
            else if (oe.GetStart() != nullptr)
            {
                const glm::dvec3 p = oe.GetStartPosition();
                AppendRingPoint(ring, gp_Pnt(p.x, p.y, p.z));
            }
        }
        RemoveConsecutiveDuplicateRingPoints(ring, 1e-6);
        if (ring.size() < 3 || RingSpanXyMm(ring) < minSpanMm)
            continue;
        for (glm::dvec3 &p : ring)
            p = ProjectPointToXyPlane(p, zPlane);
        rings.push_back(std::move(ring));
    }
    return rings;
}


struct FacePlaneSourceWire
{
    TopoDS_Wire wire;
    double absArea = 0.0;
};

// Pre-offset cache: stores the inset-independent parts of BuildStrutSetup (OCCT face analysis,
// source wire collection, coordinate transforms) keyed without insetMm so they survive inset
// slider changes and only the fast offset steps need rerunning.
struct PreOffsetCacheKey
{
    const Face *face;
    double chordTolMm;
    double minFeatureMm;
    bool operator==(const PreOffsetCacheKey &o) const noexcept
    {
        return face == o.face && chordTolMm == o.chordTolMm && minFeatureMm == o.minFeatureMm;
    }
};
struct PreOffsetCacheKeyHash
{
    std::size_t operator()(const PreOffsetCacheKey &k) const noexcept
    {
        std::size_t h = std::hash<const Face *>{}(k.face);
        h ^= std::hash<long long>{}(static_cast<long long>(k.chordTolMm * 1e6)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<long long>{}(static_cast<long long>(k.minFeatureMm * 1e6)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};
struct PreOffsetData
{
    std::vector<FacePlaneSourceWire> sourceWires;
    gp_Trsf toFlatInv;
};
std::unordered_map<PreOffsetCacheKey, PreOffsetData, PreOffsetCacheKeyHash> &PreOffsetCache()
{
    static std::unordered_map<PreOffsetCacheKey, PreOffsetData, PreOffsetCacheKeyHash> cache;
    return cache;
}

// Caches whether a candidate strut between wall anchors i and j actually fits once clipped
// against its own walls' real geometry (BuildStrutQuadByWallClip) — an OCCT boolean, not a cheap
// 2D test, so worth not rerunning across the multiple call sites (production geometry, preview
// overlay, candidate-list overlay) that each independently walk every anchor pair for the same
// face/params within the same user action. Keyed on anchor INDEX, not position: anchors are
// recomputed deterministically from the same face geometry and params each call, so the same
// (i, j) refers to the same logical wall pair as long as insetMm/chordTolMm haven't changed —
// invalidated below alongside the other caches when they do.
struct StrutQuadCacheKey
{
    const Face *face;
    int i;
    int j;
    double insetMm;
    double chordTolMm;
    bool operator==(const StrutQuadCacheKey &o) const noexcept
    {
        return face == o.face && i == o.i && j == o.j && insetMm == o.insetMm &&
               chordTolMm == o.chordTolMm;
    }
};
struct StrutQuadCacheKeyHash
{
    std::size_t operator()(const StrutQuadCacheKey &k) const noexcept
    {
        std::size_t h = std::hash<const Face *>{}(k.face);
        h ^= std::hash<int>{}(k.i) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(k.j) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<long long>{}(static_cast<long long>(k.insetMm * 1e6)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<long long>{}(static_cast<long long>(k.chordTolMm * 1e6)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};
// Value is {valid, taperPenalty} — see StrutFitResult/BuildStrutQuadByWallClip's outTaperPenalty.
std::unordered_map<StrutQuadCacheKey, std::pair<bool, double>, StrutQuadCacheKeyHash> &StrutQuadValidityCache()
{
    static std::unordered_map<StrutQuadCacheKey, std::pair<bool, double>, StrutQuadCacheKeyHash> cache;
    return cache;
}

std::vector<FacePlaneSourceWire> CollectSortedFacePlaneSourceWires(const TopoDS_Face &occtFace,
                                                                   const BakeParams &params,
                                                                   const glm::dvec3 &basisNormal)
{
    std::vector<FacePlaneSourceWire> holeWires;
    FacePlaneSourceWire outerWire;
    outerWire.absArea = 0.0;

    const TopoDS_Wire occtOuter = BRepTools::OuterWire(occtFace);
    const bool haveOcctOuter = !occtOuter.IsNull();

    for (TopExp_Explorer exWire(occtFace, TopAbs_WIRE); exWire.More(); exWire.Next())
    {
        const TopoDS_Wire wire = TopoDS::Wire(exWire.Current());
        const std::vector<glm::dvec3> ring = DiscretizeWireToRing3D(wire, params.chordTolMm);
        if (ring.size() < 3)
            continue;
        const FacePlaneSourceWire entry{wire, std::abs(SignedArea2D(ring, basisNormal))};
        if (haveOcctOuter && wire.IsSame(occtOuter))
            outerWire = entry;
        else
            holeWires.push_back(entry);
    }

    std::vector<FacePlaneSourceWire> sourceWires;
    if (!outerWire.wire.IsNull())
    {
        sourceWires.push_back(outerWire);
    }
    else if (!holeWires.empty())
    {
        auto largest = std::max_element(holeWires.begin(), holeWires.end(),
                                        [](const FacePlaneSourceWire &a, const FacePlaneSourceWire &b)
                                        { return a.absArea < b.absArea; });
        sourceWires.push_back(*largest);
        holeWires.erase(largest);
    }

    std::sort(holeWires.begin(), holeWires.end(),
              [](const FacePlaneSourceWire &a, const FacePlaneSourceWire &b) { return a.absArea > b.absArea; });
    sourceWires.insert(sourceWires.end(), holeWires.begin(), holeWires.end());
    return sourceWires;
}


bool StructureDebugTraceHoles()
{
    if (!StructureDebugEnabled())
        return false;
    const char *trace = std::getenv("CAD_DEBUG_STRUCTURE_TRACE");
    return trace == nullptr || trace[0] == '\0' || !(trace[0] == '0' && trace[1] == '\0');
}

void LogHoleRingIdentity(size_t holeIndex, size_t sourceWireIndex, const std::vector<glm::dvec3> &ring,
                         const glm::dvec3 &basisNormal, const char *stage)
{
    if (ring.size() < 3)
    {
        std::cerr << "Structure hole trace " << stage << " hole[" << holeIndex << "] sourceWire="
                  << sourceWireIndex << ": (empty ring)\n";
        return;
    }
    const double area = std::abs(SignedArea2D(ring, basisNormal));
    const glm::dvec2 c = RingCentroidXy(ring);
    const double roundness = RingRoundness(ring, basisNormal);
    std::cerr << "Structure hole trace " << stage << " hole[" << holeIndex << "] sourceWire="
              << sourceWireIndex << " areaMm2=" << area << " centroid=(" << c.x << "," << c.y
              << ") spanMm=" << RingMaxSpanMm(ring) << " roundness=" << roundness;
    if (roundness > 0.88)
        std::cerr << " [likely circle]";
    else if (roundness < 0.75)
        std::cerr << " [likely rect/oval]";
    std::cerr << " pts=" << ring.size() << "\n";
}


void LogHoleVoidSurvivalAfterCut(const std::vector<std::vector<glm::dvec3>> &holeRings,
                                 const std::vector<std::vector<glm::dvec3>> &resultRings,
                                 const glm::dvec3 &basisNormal, size_t lastCutHoleIndex)
{
    glm::dvec3 u;
    glm::dvec3 v;
    BuildPlanarBasis(basisNormal, u, v);

    std::cerr << "Structure hole trace after cut hole[" << lastCutHoleIndex
              << "]: resultRings=" << resultRings.size() << " inner voids:\n";
    for (size_t ri = 1; ri < resultRings.size(); ++ri)
    {
        const double area =
            resultRings[ri].size() >= 3 ? std::abs(SignedArea2D(resultRings[ri], basisNormal)) : 0.0;
        const glm::dvec2 c = RingCentroidXy(resultRings[ri]);
        std::cerr << "  result inner ring" << ri << " areaMm2=" << area << " centroid=(" << c.x << ","
                  << c.y << ") roundness=" << RingRoundness(resultRings[ri], basisNormal) << "\n";
    }

    for (size_t hi = 0; hi < holeRings.size(); ++hi)
    {
        if (holeRings[hi].size() < 3)
            continue;
        const glm::dvec2 c = RingCentroidXy(holeRings[hi]);
        const int innerRing = FindInnerRingContainingPoint(c, resultRings, basisNormal);
        const bool inMaterial =
            resultRings.empty() ? false
                                : PointInRingPlanar(c, resultRings.front(), u, v) && innerRing < 0;

        std::cerr << "  hole[" << hi << "] centroid in inner void ring=";
        if (innerRing < 0)
            std::cerr << "NONE";
        else
            std::cerr << innerRing;
        if (inMaterial)
            std::cerr << " (WARNING: centroid still in solid — cut missed or void merged away)";
        if (hi <= lastCutHoleIndex && innerRing < 0 && !inMaterial)
            std::cerr << " (outside outer — check projection)";
        std::cerr << "\n";
    }

    for (size_t a = 0; a < holeRings.size(); ++a)
    {
        for (size_t b = a + 1; b < holeRings.size(); ++b)
        {
            const glm::dvec2 ca = RingCentroidXy(holeRings[a]);
            const glm::dvec2 cb = RingCentroidXy(holeRings[b]);
            const int ringA = FindInnerRingContainingPoint(ca, resultRings, basisNormal);
            const int ringB = FindInnerRingContainingPoint(cb, resultRings, basisNormal);
            if (ringA >= 0 && ringA == ringB)
                std::cerr << "  MERGE: hole[" << a << "] and hole[" << b << "] share inner ring" << ringA
                          << "\n";
        }
    }
}


bool SegmentOnFootprintBoundary(const glm::dvec3 &a, const glm::dvec3 &b, const std::vector<glm::dvec3> &outerRing,
                                const std::vector<std::vector<glm::dvec3>> &holeRings,
                                const glm::dvec3 &basisNormal, const glm::dvec3 &u, const glm::dvec3 &v,
                                double probeMm)
{
    const glm::dvec3 tangent = b - a;
    const double tangentLen = glm::length(tangent);
    if (tangentLen < 1e-9)
        return false;

    glm::dvec3 perp = glm::cross(basisNormal, tangent / tangentLen);
    const double perpLen = glm::length(perp);
    if (perpLen < 1e-9)
        return false;
    perp *= probeMm / perpLen;

    const glm::dvec3 mid = (a + b) * 0.5;
    const glm::dvec2 pPlus(glm::dot(mid + perp, u), glm::dot(mid + perp, v));
    const glm::dvec2 pMinus(glm::dot(mid - perp, u), glm::dot(mid - perp, v));
    return InsideFootprintPlanar(pPlus, outerRing, holeRings, u, v) !=
           InsideFootprintPlanar(pMinus, outerRing, holeRings, u, v);
}

std::vector<std::pair<glm::vec3, glm::vec3>> BuildFootprintBoundaryPreviewLines(
    const std::vector<std::vector<glm::dvec3>> &rings, const glm::dvec3 &basisNormal, double probeMm,
    const glm::dvec3 &previewLift)
{
    std::vector<std::pair<glm::vec3, glm::vec3>> segments;
    if (rings.empty() || rings.front().size() < 3)
        return segments;

    std::vector<glm::dvec3> outerRing = rings.front();
    if (SignedArea2D(outerRing, basisNormal) < 0.0)
        std::reverse(outerRing.begin(), outerRing.end());

    std::vector<std::vector<glm::dvec3>> holeRings;
    if (rings.size() > 1)
    {
        holeRings.reserve(rings.size() - 1);
        for (size_t i = 1; i < rings.size(); ++i)
        {
            if (rings[i].size() < 3)
                continue;
            std::vector<glm::dvec3> hole = rings[i];
            if (SignedArea2D(hole, basisNormal) > 0.0)
                std::reverse(hole.begin(), hole.end());
            holeRings.push_back(std::move(hole));
        }
    }

    glm::dvec3 u;
    glm::dvec3 v;
    BuildPlanarBasis(basisNormal, u, v);

    for (const std::vector<glm::dvec3> &ring : rings)
    {
        if (ring.size() < 3)
            continue;
        for (size_t i = 0; i < ring.size(); ++i)
        {
            const size_t j = (i + 1) % ring.size();
            if (!SegmentOnFootprintBoundary(ring[i], ring[j], outerRing, holeRings, basisNormal, u, v, probeMm))
                continue;
            const glm::dvec3 a = ring[i] + previewLift;
            const glm::dvec3 b = ring[j] + previewLift;
            segments.push_back({glm::vec3(a.x, a.y, a.z), glm::vec3(b.x, b.y, b.z)});
        }
    }
    return segments;
}

glm::dvec2 RingCentroidPlanar(const std::vector<glm::dvec3> &ring, const glm::dvec3 &u, const glm::dvec3 &v)
{
    glm::dvec2 c(0.0);
    if (ring.empty())
        return c;
    for (const glm::dvec3 &p : ring)
    {
        c.x += glm::dot(p, u);
        c.y += glm::dot(p, v);
    }
    c /= static_cast<double>(ring.size());
    return c;
}

bool HoleOutsetRingsOverlap(const std::vector<std::vector<glm::dvec3>> &holeRings,
                            const glm::dvec3 &basisNormal)
{
    if (holeRings.size() < 2)
        return false;
    glm::dvec3 u;
    glm::dvec3 v;
    BuildPlanarBasis(basisNormal, u, v);
    for (size_t i = 0; i < holeRings.size(); ++i)
    {
        for (size_t j = i + 1; j < holeRings.size(); ++j)
        {
            const glm::dvec2 ci = RingCentroidPlanar(holeRings[i], u, v);
            const glm::dvec2 cj = RingCentroidPlanar(holeRings[j], u, v);
            if (PointInRingPlanar(ci, holeRings[j], u, v) || PointInRingPlanar(cj, holeRings[i], u, v))
                return true;
            for (const glm::dvec3 &p : holeRings[i])
            {
                const glm::dvec2 pt(glm::dot(p, u), glm::dot(p, v));
                if (PointInRingPlanar(pt, holeRings[j], u, v))
                    return true;
            }
            for (const glm::dvec3 &p : holeRings[j])
            {
                const glm::dvec2 pt(glm::dot(p, u), glm::dot(p, v));
                if (PointInRingPlanar(pt, holeRings[i], u, v))
                    return true;
            }
        }
    }
    return false;
}

bool FaceNormalParallelTo(const TopoDS_Face &face, const glm::dvec3 &dirHint, double minDot = 0.95)
{
    Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
    if (surf.IsNull())
        return false;
    Standard_Real uMin = 0.0;
    Standard_Real uMax = 0.0;
    Standard_Real vMin = 0.0;
    Standard_Real vMax = 0.0;
    surf->Bounds(uMin, uMax, vMin, vMax);
    gp_Pnt p;
    gp_Vec d1u;
    gp_Vec d1v;
    surf->D1((uMin + uMax) * 0.5, (vMin + vMax) * 0.5, p, d1u, d1v);
    gp_Vec normal = d1u ^ d1v;
    if (face.Orientation() == TopAbs_REVERSED)
        normal.Reverse();
    if (normal.Magnitude() < 1e-12)
        return false;
    gp_Dir dir(dirHint.x, dirHint.y, dirHint.z);
    return normal.Dot(dir) >= minDot;
}

double PlanarShapeAreaMm2(const TopoDS_Shape &shape, const glm::dvec3 &basisNormal)
{
    if (shape.IsNull())
        return 0.0;
    double total = 0.0;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next())
    {
        const TopoDS_Face face = TopoDS::Face(ex.Current());
        if (!FaceNormalParallelTo(face, basisNormal))
            continue;
        GProp_GProps props;
        BRepGProp::SurfaceProperties(face, props);
        total += props.Mass();
    }
    return total;
}

struct BooleanTrimOutcome
{
    std::vector<std::vector<glm::dvec3>> rings;
    TopoDS_Shape resultShape;
    size_t holesRequested = 0;
    size_t holesCutOk = 0;
    double outerAreaMm2 = 0.0;
    double remainingAreaMm2 = 0.0;
};

bool BooleanTrimOutcomeValid(const BooleanTrimOutcome &outcome,
                             const std::vector<std::vector<glm::dvec3>> &makeFaceRings)
{
    if (outcome.rings.empty())
        return false;
    if (outcome.holesRequested == 0)
        return true;
    if (outcome.holesCutOk < outcome.holesRequested)
        return false;
    // OCCT GProp area on coplanar face booleans can stay unchanged even when void topology
    // merged correctly — compare ring counts instead of surface area.
    if (!makeFaceRings.empty() && outcome.rings.size() > makeFaceRings.size())
        return false;
    return true;
}

TopoDS_Shape FuseCoplanarFaces(const TopoDS_Shape &a, const TopoDS_Shape &b)
{
    if (a.IsNull())
        return b;
    if (b.IsNull())
        return a;
    BRepAlgoAPI_Fuse fuse(a, b);
    fuse.SetFuzzyValue(1e-3);
    fuse.Build();
    if (!fuse.IsDone())
        return a;
    ShapeUpgrade_UnifySameDomain unify(fuse.Shape(), Standard_True, Standard_True, Standard_True);
    unify.Build();
    return unify.Shape().IsNull() ? fuse.Shape() : unify.Shape();
}

TopoDS_Shape FuseHoleOutsetFaceRegions(const std::vector<TopoDS_Wire> &holeWires)
{
    TopoDS_Shape fused;
    for (const TopoDS_Wire &wire : holeWires)
    {
        if (wire.IsNull())
            continue;
        BRepBuilderAPI_MakeFace mkHole(wire);
        if (!mkHole.IsDone())
            continue;
        fused = FuseCoplanarFaces(fused, mkHole.Face());
    }
    return fused;
}

std::vector<TopoDS_Wire> CollectOuterWiresFromFaceShape(const TopoDS_Shape &shape)
{
    std::vector<TopoDS_Wire> wires;
    if (shape.IsNull())
        return wires;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next())
    {
        const TopoDS_Wire outer = BRepTools::OuterWire(TopoDS::Face(ex.Current()));
        if (!outer.IsNull())
            wires.push_back(outer);
    }
    return wires;
}

std::vector<std::vector<glm::dvec3>> ExtractSortedRingsFromPlanarShape(const TopoDS_Shape &shape,
                                                                      const glm::dvec3 &basisNormal,
                                                                      double chordTolMm)
{
    struct RingEntry
    {
        double absArea = 0.0;
        std::vector<glm::dvec3> ring;
    };
    std::vector<RingEntry> entries;
    for (TopExp_Explorer exFace(shape, TopAbs_FACE); exFace.More(); exFace.Next())
    {
        const TopoDS_Face face = TopoDS::Face(exFace.Current());
        if (!FaceNormalParallelTo(face, basisNormal))
            continue;
        for (TopExp_Explorer exWire(face, TopAbs_WIRE); exWire.More(); exWire.Next())
        {
            const std::vector<glm::dvec3> ring =
                DiscretizeWireToRing3D(TopoDS::Wire(exWire.Current()), chordTolMm);
            if (ring.size() < 3)
                continue;
            entries.push_back({std::abs(SignedArea2D(ring, basisNormal)), ring});
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const RingEntry &a, const RingEntry &b) { return a.absArea > b.absArea; });
    std::vector<std::vector<glm::dvec3>> rings;
    rings.reserve(entries.size());
    constexpr double kSliverSpanMm = 0.25;
    for (RingEntry &entry : entries)
    {
        if (entry.ring.size() < 4)
            continue;
        const double span = RingMaxSpanMm(entry.ring);
        if (span < kSliverSpanMm)
            continue;
        constexpr double kSliverAreaMm2 = 0.05;
        if (entry.absArea < kSliverAreaMm2)
            continue;
        rings.push_back(std::move(entry.ring));
    }
    return rings;
}

std::vector<std::vector<glm::dvec3>> ExtractRingsFromInsetOuterAndHoleWires(
    const TopoDS_Wire &insetOuter, const std::vector<TopoDS_Wire> &holeWires,
    const std::vector<size_t> &holeIndices, const glm::dvec3 &basisNormal, double chordTolMm)
{
    if (insetOuter.IsNull())
        return {};
    if (holeIndices.empty())
    {
        const std::vector<glm::dvec3> outerRing = DiscretizeWireToRing3D(insetOuter, chordTolMm);
        if (outerRing.size() >= 3)
            return {outerRing};
        return {};
    }

    BRepBuilderAPI_MakeFace mkFace(insetOuter);
    if (!mkFace.IsDone())
        return {};
    for (const size_t holeIndex : holeIndices)
    {
        if (holeIndex >= holeWires.size() || holeWires[holeIndex].IsNull())
            continue;
        // Discretize to polygon before Add — offset results for ellipse/B-spline wires can have
        // slight z-drift that causes MakeFace::Add to reject the original wire.
        const std::vector<glm::dvec3> ring = DiscretizeWireToRing3D(holeWires[holeIndex], chordTolMm);
        if (ring.size() < 3)
            continue;
        BRepBuilderAPI_MakePolygon poly;
        for (const glm::dvec3 &p : ring)
            poly.Add(gp_Pnt(p.x, p.y, p.z));
        poly.Close();
        if (poly.IsDone())
            mkFace.Add(poly.Wire());
    }
    if (!mkFace.IsDone())
        return {};
    return ExtractSortedRingsFromPlanarShape(mkFace.Face(), basisNormal, chordTolMm);
}

std::vector<size_t> ResolvePreviewHoleIndices(const std::vector<TopoDS_Wire> &holeWires, int maxCuts,
                                              int cutOnly)
{
    std::vector<size_t> indices;
    if (cutOnly >= 0)
    {
        if (static_cast<size_t>(cutOnly) < holeWires.size())
            indices.push_back(static_cast<size_t>(cutOnly));
        return indices;
    }
    const size_t count = maxCuts >= 0 ? std::min(static_cast<size_t>(maxCuts), holeWires.size())
                                      : holeWires.size();
    indices.reserve(count);
    for (size_t i = 0; i < count; ++i)
        indices.push_back(i);
    return indices;
}

BooleanTrimOutcome TryBooleanTrimPreview(const TopoDS_Wire &insetOuter,
                                         const std::vector<TopoDS_Wire> &holeWires,
                                         const std::vector<size_t> &holeIndices, const BakeParams &params,
                                         const glm::dvec3 &basisNormal)
{
    BooleanTrimOutcome outcome;
    if (insetOuter.IsNull())
        return outcome;

    // Compute face plane z from the outer ring so polygon hole faces are coplanar.
    // BRepOffsetAPI_MakeOffset results can have slight z-drift; forcing the same z as the
    // outer face prevents BRepBuilderAPI_MakeFace from failing on the hole polygons.
    const std::vector<glm::dvec3> outerRingPts = DiscretizeWireToRing3D(insetOuter, params.chordTolMm);
    double zFace = 0.0;
    if (!outerRingPts.empty())
    {
        for (const glm::dvec3 &p : outerRingPts) zFace += p.z;
        zFace /= static_cast<double>(outerRingPts.size());
    }

    BRepBuilderAPI_MakeFace mkOuter(insetOuter);
    if (!mkOuter.IsDone())
        return outcome;
    TopoDS_Shape remaining = mkOuter.Face();
    outcome.outerAreaMm2 = PlanarShapeAreaMm2(remaining, basisNormal);

    if (holeIndices.empty())
    {
        outcome.rings = ExtractSortedRingsFromPlanarShape(remaining, basisNormal, params.chordTolMm);
        outcome.remainingAreaMm2 = outcome.outerAreaMm2;
        return outcome;
    }

    for (const size_t holeIndex : holeIndices)
    {
        if (holeIndex >= holeWires.size())
            continue;
        const TopoDS_Wire &holeWire = holeWires[holeIndex];
        if (holeWire.IsNull())
            continue;
        ++outcome.holesRequested;

        // Bind the hole face to an explicit plane at zFace (matching the outer face) instead
        // of discretizing the wire to a polygon. BRepOffsetAPI_MakeOffset results can have
        // slight z-drift that makes BRepBuilderAPI_MakeFace(wire) reject the wire outright;
        // giving it an explicit reference plane tolerates that drift while preserving the
        // wire's analytic edges (arcs, B-splines, ...) through the cut.
        const gp_Pln holePlane(gp_Pnt(0.0, 0.0, zFace), gp_Dir(basisNormal.x, basisNormal.y, basisNormal.z));
        BRepBuilderAPI_MakeFace mkHole(holePlane, holeWire);
        if (!mkHole.IsDone())
            continue;

        BRepAlgoAPI_Cut cut(remaining, mkHole.Face());
        cut.SetFuzzyValue(1e-3);
        cut.Build();
        if (!cut.IsDone())
            continue;
        remaining = cut.Shape();
        ++outcome.holesCutOk;
    }

    outcome.remainingAreaMm2 = PlanarShapeAreaMm2(remaining, basisNormal);
    if (remaining.IsNull())
        return outcome;
    outcome.resultShape = remaining;
    outcome.rings = ExtractSortedRingsFromPlanarShape(remaining, basisNormal, params.chordTolMm);
    return outcome;
}

std::vector<std::vector<glm::dvec3>> TryBooleanTrimPreviewRings(
    const TopoDS_Wire &insetOuter, const std::vector<TopoDS_Wire> &holeWires,
    const std::vector<size_t> &holeIndices, const BakeParams &params, const glm::dvec3 &basisNormal)
{
    return TryBooleanTrimPreview(insetOuter, holeWires, holeIndices, params, basisNormal).rings;
}

/// Fuses outset hole regions on the face plane (2D face fuse, not thin-solid extrusion).
TopoDS_Face BuildFacePlaneHoleUnionFace(const std::vector<FacePlaneSourceWire> &sourceWires,
                                        const BakeParams &params, const glm::dvec3 &extrudeDir)
{
    std::vector<TopoDS_Wire> holeWires;
    for (size_t i = 1; i < sourceWires.size(); ++i)
    {
        const TopoDS_Wire outsetHole =
            OffsetHoleWireOutward(sourceWires[i].wire, params.insetMm, extrudeDir, params.chordTolMm);
        if (!outsetHole.IsNull())
            holeWires.push_back(outsetHole);
    }
    const TopoDS_Shape fused = FuseHoleOutsetFaceRegions(holeWires);
    if (fused.IsNull())
        return TopoDS_Face();

    double bestArea = 0.0;
    TopoDS_Face best;
    for (TopExp_Explorer ex(fused, TopAbs_FACE); ex.More(); ex.Next())
    {
        const TopoDS_Face face = TopoDS::Face(ex.Current());
        if (!FaceNormalParallelTo(face, extrudeDir))
            continue;
        GProp_GProps props;
        BRepGProp::SurfaceProperties(face, props);
        if (props.Mass() > bestArea)
        {
            bestArea = props.Mass();
            best = face;
        }
    }
    return best;
}

std::vector<std::vector<glm::dvec3>> ExtractSortedRingsFromPlanarFace(const TopoDS_Face &face,
                                                                      const glm::dvec3 &basisNormal,
                                                                      double chordTolMm)
{
    return ExtractSortedRingsFromPlanarShape(face, basisNormal, chordTolMm);
}

/// Builds outer-inset minus hole-outset voids; returns boundary rings (outer first).
/// Primary path: MakeFace(outer + polygon hole wires) — tolerates B-spline/ellipse offset wires.
/// Fallback: sequential OCCT boolean cut. Last resort: outer ring + raw hole rings.
std::vector<std::vector<glm::dvec3>> BuildTrimmedPreviewRingsFromFootprintFace(
    const TopoDS_Wire &insetOuter, const std::vector<FacePlaneSourceWire> &sourceWires,
    const std::vector<TopoDS_Wire> &holeWires, const std::vector<std::vector<glm::dvec3>> &holeRings,
    const BakeParams &params, const glm::dvec3 &basisNormal)
{
    (void)sourceWires;
    if (insetOuter.IsNull())
        return {};

    const bool debug = StructureDebugEnabled();
    const bool traceHoles = StructureDebugTraceHoles();
    const int maxCuts = StructureDebugEnvInt("CAD_DEBUG_STRUCTURE_MAX_CUTS", -1);
    const int cutOnly = StructureDebugEnvInt("CAD_DEBUG_STRUCTURE_CUT_ONLY", -1);
    const bool stepMode = StructureDebugStepCutMode();

    const std::vector<size_t> holeIndices = ResolvePreviewHoleIndices(holeWires, maxCuts, cutOnly);

    if (traceHoles)
    {
        for (size_t i = 0; i < holeRings.size(); ++i)
            LogHoleRingIdentity(i, i, holeRings[i], basisNormal, "outset-ready-for-cut");
        const std::vector<glm::dvec3> outerRingOnly =
            DiscretizeWireToRing3D(insetOuter, params.chordTolMm);
        if (outerRingOnly.size() >= 3)
        {
            const double outerArea = std::abs(SignedArea2D(outerRingOnly, basisNormal));
            std::cerr << "Structure hole trace outer-inset areaMm2=" << outerArea << " pts="
                      << outerRingOnly.size() << "\n";
        }
    }

    if (debug && stepMode)
    {
        const std::vector<std::vector<glm::dvec3>> outerOnly =
            ExtractRingsFromInsetOuterAndHoleWires(insetOuter, holeWires, {}, basisNormal,
                                                   params.chordTolMm);
        std::cerr << "Structure footprint step 0 (outer inset only): rings=" << outerOnly.size();
        if (maxCuts >= 0)
            std::cerr << " maxCuts=" << maxCuts;
        if (cutOnly >= 0)
            std::cerr << " cutOnly=" << cutOnly;
        std::cerr << "\n";

        for (size_t step = 1; step <= holeIndices.size(); ++step)
        {
            const std::vector<size_t> stepIndices(holeIndices.begin(),
                                                  holeIndices.begin() + static_cast<std::ptrdiff_t>(step));
            const std::vector<std::vector<glm::dvec3>> stepRings =
                ExtractRingsFromInsetOuterAndHoleWires(insetOuter, holeWires, stepIndices, basisNormal,
                                                       params.chordTolMm);
            std::cerr << "Structure footprint step " << step << " (+hole[" << stepIndices.back()
                      << "]): rings=" << stepRings.size();
            for (size_t ri = 0; ri < stepRings.size(); ++ri)
                std::cerr << " ring" << ri << " pts=" << stepRings[ri].size();
            std::cerr << "\n";
            if (traceHoles)
                LogHoleVoidSurvivalAfterCut(holeRings, stepRings, basisNormal, stepIndices.back());
        }
    }

    // Primary: sequential boolean cut with polygon hole faces (handles holes that extend outside
    // the outer inset, which MakeFace::Add cannot accept).
    const BooleanTrimOutcome booleanOutcome =
        TryBooleanTrimPreview(insetOuter, holeWires, holeIndices, params, basisNormal);

    // Fallback: MakeFace + polygon hole wires (when boolean gives wrong ring count).
    const std::vector<std::vector<glm::dvec3>> makeFaceRings =
        ExtractRingsFromInsetOuterAndHoleWires(insetOuter, holeWires, holeIndices, basisNormal,
                                               params.chordTolMm);

    const bool booleanUsable = BooleanTrimOutcomeValid(booleanOutcome, makeFaceRings);

    std::vector<std::vector<glm::dvec3>> rings =
        booleanUsable ? booleanOutcome.rings : makeFaceRings;

    // Last resort: outer ring + raw hole rings — avoids all OCCT topology, always visualises.
    if (rings.empty())
    {
        std::vector<glm::dvec3> outerRing = DiscretizeWireToRing3D(insetOuter, params.chordTolMm);
        if (outerRing.size() >= 3)
        {
            rings.push_back(std::move(outerRing));
            for (const size_t holeIndex : holeIndices)
            {
                if (holeIndex < holeRings.size() && holeRings[holeIndex].size() >= 3)
                    rings.push_back(holeRings[holeIndex]);
            }
        }
        if ((debug || traceHoles) && !rings.empty())
            std::cerr << "Structure footprint: direct ring assembly last resort rings=" << rings.size() << "\n";
    }

    if (debug || traceHoles)
    {
        std::cerr << "Structure footprint: boolean rings=" << booleanOutcome.rings.size()
                  << " holesCut=" << booleanOutcome.holesCutOk << "/" << booleanOutcome.holesRequested
                  << " usable=" << booleanUsable << " makeFaceRings=" << makeFaceRings.size()
                  << " final=" << rings.size() << "\n";
    }

    return rings;
}

TopoDS_Face MakeFlatFaceFromOuterAndHoles(const std::vector<glm::dvec3> &outer,
                                          const std::vector<std::vector<glm::dvec3>> &holes, double zBottom)
{
    const TopoDS_Wire outerWire = MakeFlatWireFromRing(outer, zBottom);
    if (outerWire.IsNull())
        return TopoDS_Face();

    BRepBuilderAPI_MakeFace mkFace(outerWire);
    for (const std::vector<glm::dvec3> &hole : holes)
    {
        const TopoDS_Wire holeWire = MakeFlatWireFromRing(hole, zBottom);
        if (!holeWire.IsNull())
            mkFace.Add(holeWire);
    }
    if (!mkFace.IsDone())
        return TopoDS_Face();
    const TopoDS_Face face = mkFace.Face();
    ValidateOutlineShapeTopology(face, "MakeFlatFaceFromOuterAndHoles");
    return face;
}

// Same as `MakeFlatFaceFromOuterAndHoles`, but builds each ring via `MakeFlatWireFromRingOrCircle`
// so untouched circular rings become exact `gp_Circ` edges instead of dense polygons (Phase 1 of
// the carve-blowup fix — see project memory). Deliberately used only for the final carve-compound
// build that feeds extrusion (`BuildCarveCompoundFromFootprint`): that's the one place curved
// edges are known-safe (they go straight into `BRepPrimAPI_MakePrism` + a 3D solid `BRepAlgoAPI_Cut`,
// not the 2D coplanar boolean-trim/offset steps where curved wires previously hit z-drift
// `MakeFace::Add` rejections — see the comments on `TryBooleanTrimPreview`/`BuildXyHoledOffsetFootprint`).
// Preview/trim code paths keep calling the original polygon-only builder untouched.
TopoDS_Face MakeFlatFaceFromOuterAndHolesPreferCircles(const std::vector<glm::dvec3> &outer,
                                                        const std::vector<std::vector<glm::dvec3>> &holes,
                                                        double zBottom, double chordTolMm)
{
    const TopoDS_Wire outerWire = MakeFlatWireFromRingOrCircle(outer, zBottom, chordTolMm);
    if (outerWire.IsNull())
        return TopoDS_Face();

    BRepBuilderAPI_MakeFace mkFace(outerWire);
    for (const std::vector<glm::dvec3> &hole : holes)
    {
        const TopoDS_Wire holeWire = MakeFlatWireFromRingOrCircle(hole, zBottom, chordTolMm);
        if (!holeWire.IsNull())
            mkFace.Add(holeWire);
    }
    if (!mkFace.IsDone())
        return TopoDS_Face();
    const TopoDS_Face face = mkFace.Face();
    if (!ValidateOutlineShapeTopology(face, "MakeFlatFaceFromOuterAndHolesPreferCircles"))
        return MakeFlatFaceFromOuterAndHoles(outer, holes, zBottom);
    return face;
}

struct FacePlaneFrame
{
    glm::dvec3 origin{0.0};
    glm::dvec3 n{0.0, 0.0, 1.0};
    glm::dvec3 u{1.0, 0.0, 0.0};
    glm::dvec3 v{0.0, 1.0, 0.0};
};

FacePlaneFrame MakeFacePlaneFrame(const Face *face)
{
    FacePlaneFrame frame;
    if (face == nullptr || face->surface == nullptr)
        return frame;
    frame.n = face->surface->GetNormal();
    const double nLen = glm::length(frame.n);
    if (nLen > 1e-12)
        frame.n /= nLen;
    BuildPlanarBasis(frame.n, frame.u, frame.v);
    for (const auto &loop : face->loops)
    {
        for (const OrientedEdge &oe : loop)
        {
            if (oe.GetStart() != nullptr)
            {
                frame.origin = oe.GetStartPosition();
                return frame;
            }
        }
    }
    return frame;
}

glm::dvec2 WorldToFaceLocalUv(const glm::dvec3 &p, const FacePlaneFrame &frame)
{
    const glm::dvec3 rel = p - frame.origin;
    return glm::dvec2(glm::dot(rel, frame.u), glm::dot(rel, frame.v));
}

glm::dvec3 FaceLocalUvToWorld(const glm::dvec2 &uv, const FacePlaneFrame &frame)
{
    return frame.origin + frame.u * uv.x + frame.v * uv.y;
}

std::vector<glm::dvec3> ProjectRingToFaceLocalFlat(const std::vector<glm::dvec3> &ring,
                                                   const FacePlaneFrame &frame)
{
    std::vector<glm::dvec3> projected;
    projected.reserve(ring.size());
    for (const glm::dvec3 &p : ring)
    {
        const glm::dvec2 uv = WorldToFaceLocalUv(p, frame);
        projected.push_back(glm::dvec3(uv.x, uv.y, 0.0));
    }
    return projected;
}

void LiftFaceLocalRingsToWorld(std::vector<std::vector<glm::dvec3>> &rings, const FacePlaneFrame &frame)
{
    for (auto &ring : rings)
    {
        for (glm::dvec3 &p : ring)
            p = FaceLocalUvToWorld(glm::dvec2(p.x, p.y), frame);
    }
}

gp_Trsf MakeFaceLocalFlatTrsf(const FacePlaneFrame &frame)
{
    const gp_Pnt origin(frame.origin.x, frame.origin.y, frame.origin.z);
    const gp_Dir nDir(frame.n.x, frame.n.y, frame.n.z);
    const gp_Dir uDir(frame.u.x, frame.u.y, frame.u.z);
    const gp_Ax3 faceAx(origin, nDir, uDir);
    const gp_Ax3 flatAx(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0), gp_Dir(1.0, 0.0, 0.0));
    gp_Trsf trsf;
    trsf.SetTransformation(faceAx, flatAx);
    return trsf;
}

TopoDS_Face TransformOcctFaceToLocalFlat(const TopoDS_Face &occtFace, const gp_Trsf &toFlat)
{
    if (occtFace.IsNull())
        return TopoDS_Face();
    BRepBuilderAPI_Transform transform(occtFace, toFlat, Standard_True);
    if (!transform.IsDone())
        return TopoDS_Face();
    const TopoDS_Shape shape = transform.Shape();
    if (shape.IsNull() || shape.ShapeType() != TopAbs_FACE)
        return TopoDS_Face();
    return TopoDS::Face(shape);
}

void TransformRingsWithTrsf(std::vector<std::vector<glm::dvec3>> &rings, const gp_Trsf &trsf)
{
    for (auto &ring : rings)
    {
        for (glm::dvec3 &p : ring)
        {
            gp_Pnt pt(p.x, p.y, p.z);
            pt.Transform(trsf);
            p = glm::dvec3(pt.X(), pt.Y(), pt.Z());
        }
    }
}

struct FaceCapBounds
{
    glm::dvec3 centroid{0.0};
    double maxZ = 0.0;
};

FaceCapBounds ComputeFaceCapBounds(const Face *face)
{
    FaceCapBounds bounds;
    if (face == nullptr)
        return bounds;
    size_t count = 0;
    for (const auto &loop : face->loops)
    {
        for (const OrientedEdge &oe : loop)
        {
            if (oe.GetStart() == nullptr)
                continue;
            const glm::dvec3 p = oe.GetStartPosition();
            bounds.centroid += p;
            bounds.maxZ = std::max(bounds.maxZ, p.z);
            ++count;
        }
    }
    if (count > 0)
        bounds.centroid /= static_cast<double>(count);
    return bounds;
}

void FlattenRingsToWorldXyAtCapTop(std::vector<std::vector<glm::dvec3>> &rings, const Face *face)
{
    const double capTopZ = ComputeFaceCapBounds(face).maxZ;
    for (auto &ring : rings)
    {
        for (glm::dvec3 &p : ring)
            p.z = capTopZ;
    }
}

void MapXyTrimmedRingsToPreviewSpace(std::vector<std::vector<glm::dvec3>> &rings, const Face *face,
                                     const FacePlaneFrame &frame, const gp_Trsf &toFlat,
                                     bool usedPolygonSource, bool mapOntoCap)
{
    if (usedPolygonSource)
    {
        for (auto &ring : rings)
        {
            for (glm::dvec3 &p : ring)
                p = FaceLocalUvToWorld(glm::dvec2(p.x, p.y), frame);
        }
    }
    else
        TransformRingsWithTrsf(rings, toFlat.Inverted());

    if (!mapOntoCap)
        FlattenRingsToWorldXyAtCapTop(rings, face);
}

std::vector<std::vector<glm::dvec3>> CollectRingsFromOcctFaceInFaceLocalFlat(const TopoDS_Face &occtFace,
                                                                             const Face *face,
                                                                             const FacePlaneFrame &frame,
                                                                             double chordTolMm,
                                                                             double minSpanMm)
{
    std::vector<std::vector<glm::dvec3>> rings;
    if (occtFace.IsNull())
        return rings;

    auto tryAddRing = [&](const TopoDS_Wire &wire)
    {
        std::vector<glm::dvec3> ring = ExtractRingPointsInWireOrder(wire, chordTolMm);
        if (ring.size() < 3 || RingMaxSpanMm(ring) < minSpanMm)
            return;
        rings.push_back(ProjectRingToFaceLocalFlat(ring, frame));
    };

    for (TopExp_Explorer exWire(occtFace, TopAbs_WIRE); exWire.More(); exWire.Next())
        tryAddRing(TopoDS::Wire(exWire.Current()));

    if (rings.empty() && face != nullptr)
    {
        for (const auto &loop : face->loops)
        {
            std::vector<glm::dvec3> ring;
            for (const OrientedEdge &oe : loop)
            {
                if (oe.edge == nullptr)
                    continue;
                if (!oe.edge->occtEdge.IsNull())
                {
                    TopoDS_Edge edge = oe.edge->occtEdge;
                    if (oe.reversed)
                        edge.Reverse();
                    CollectPointsFromEdge(edge, chordTolMm, ring);
                }
                else if (oe.GetStart() != nullptr)
                {
                    const glm::dvec3 p = oe.GetStartPosition();
                    AppendRingPoint(ring, gp_Pnt(p.x, p.y, p.z));
                }
            }
            RemoveConsecutiveDuplicateRingPoints(ring, 1e-6);
            if (ring.size() < 3 || RingMaxSpanMm(ring) < minSpanMm)
                continue;
            rings.push_back(ProjectRingToFaceLocalFlat(ring, frame));
        }
    }
    return rings;
}

TopoDS_Face BuildFacePlaneLocalFlatSourceFace(const Face *face, const TopoDS_Face &occtFace,
                                              const FacePlaneFrame &frame, double chordTolMm,
                                              double minSpanMm)
{
    std::vector<std::vector<glm::dvec3>> rings =
        CollectRingsFromOcctFaceInFaceLocalFlat(occtFace, face, frame, chordTolMm, minSpanMm);
    if (rings.empty())
        return TopoDS_Face();

    std::vector<glm::dvec3> outer;
    std::vector<std::vector<glm::dvec3>> holes;
    ClassifyOuterAndHoles(rings, kWorldUpNormal, outer, holes);
    constexpr double kLocalPlaneZ = 0.0;
    return MakeFlatFaceFromOuterAndHoles(outer, holes, kLocalPlaneZ);
}

TopoDS_Face BuildXyProjectedSourceFace(const Face *face, const TopoDS_Face &occtFace, double zPlane,
                                       double chordTolMm, double minSpanMm)
{
    if (face != nullptr)
    {
        const FacePlaneFrame frame = MakeFacePlaneFrame(face);
        const gp_Trsf toFlat = MakeFaceLocalFlatTrsf(frame);
        const TopoDS_Face localFace = TransformOcctFaceToLocalFlat(occtFace, toFlat);
        if (!localFace.IsNull())
            return localFace;
        const TopoDS_Face polygonFallback =
            BuildFacePlaneLocalFlatSourceFace(face, occtFace, frame, chordTolMm, minSpanMm);
        if (!polygonFallback.IsNull())
            return polygonFallback;
    }

    std::vector<std::vector<glm::dvec3>> rings =
        CollectRingsFromShape(occtFace, chordTolMm, zPlane, minSpanMm);
    if (rings.empty())
        rings = CollectRingsFromFaceLoops(face, zPlane, chordTolMm, minSpanMm);
    if (rings.empty())
        return TopoDS_Face();

    std::vector<glm::dvec3> outer;
    std::vector<std::vector<glm::dvec3>> holes;
    ClassifyOuterAndHoles(rings, kWorldUpNormal, outer, holes);
    return MakeFlatFaceFromOuterAndHoles(outer, holes, zPlane);
}

TopoDS_Face BuildXyProjectedSourceFace(const TopoDS_Face &occtFace, double zPlane, double chordTolMm,
                                       double minSpanMm)
{
    return BuildXyProjectedSourceFace(nullptr, occtFace, zPlane, chordTolMm, minSpanMm);
}

TopoDS_Shape BuildXyHoledOffsetFootprint(const Face *face, const TopoDS_Face &occtFace,
                                         const BakeParams &params)
{
    const TopoDS_Face xySource =
        BuildXyProjectedSourceFace(face, occtFace, kXyPlaneZ, params.chordTolMm, params.minFeatureMm);
    if (xySource.IsNull())
        return TopoDS_Shape();

    struct SourceWire
    {
        TopoDS_Wire wire;
        double absArea = 0.0;
    };
    std::vector<SourceWire> sourceWires;
    for (TopExp_Explorer exWire(xySource, TopAbs_WIRE); exWire.More(); exWire.Next())
    {
        const TopoDS_Wire wire = TopoDS::Wire(exWire.Current());
        const std::vector<glm::dvec3> ring = DiscretizeWireToXyRing(wire, kXyPlaneZ, params.chordTolMm);
        if (ring.size() < 3)
            continue;
        sourceWires.push_back({wire, std::abs(SignedArea2D(ring, kWorldUpNormal))});
    }
    if (sourceWires.empty())
        return TopoDS_Shape();

    std::sort(sourceWires.begin(), sourceWires.end(),
              [](const SourceWire &a, const SourceWire &b) { return a.absArea > b.absArea; });

    const TopoDS_Wire insetOuter =
        OffsetOuterWireInward(sourceWires.front().wire, params.insetMm, kWorldUpNormal, params.chordTolMm);
    if (insetOuter.IsNull())
        return TopoDS_Shape();

    BRepBuilderAPI_MakeFace mkFace(insetOuter);
    for (size_t i = 1; i < sourceWires.size(); ++i)
    {
        const TopoDS_Wire outsetHole = OffsetHoleWireOutward(sourceWires[i].wire, params.insetMm, kWorldUpNormal,
                                                             params.chordTolMm);
        if (outsetHole.IsNull())
            continue;
        // Discretize to polygon at z=0 — offset results for ellipse/B-spline wires can have
        // slight z-drift that causes MakeFace::Add to reject the original wire.
        const std::vector<glm::dvec3> holeRing = DiscretizeWireToXyRing(outsetHole, kXyPlaneZ, params.chordTolMm);
        if (holeRing.size() < 3)
            continue;
        const TopoDS_Wire holePolyWire = MakeFlatWireFromRing(holeRing, kXyPlaneZ);
        if (!holePolyWire.IsNull())
            mkFace.Add(holePolyWire);
    }
    if (!mkFace.IsDone())
        return TopoDS_Shape();
    return mkFace.Face();
}

/// Offset + trim on a planar wire set. `planeNormal` is +Z for face-local flat footprints.
/// When `makeFaceOnlyTrim` is true, skip OCCT boolean (can hang on some holed caps) and use MakeFace.
std::vector<std::vector<glm::dvec3>> BuildTrimmedPreviewRingsFromSourceWires(
    const std::vector<FacePlaneSourceWire> &sourceWires, const BakeParams &params,
    const glm::dvec3 &planeNormal, bool makeFaceOnlyTrim = false)
{
    std::vector<std::vector<glm::dvec3>> rings;
    if (sourceWires.empty())
        return rings;

    const TopoDS_Wire insetOuter =
        OffsetOuterWireInward(sourceWires.front().wire, params.insetMm, planeNormal, params.chordTolMm);
    if (insetOuter.IsNull())
        return rings;

    if (sourceWires.size() <= 1)
    {
        std::vector<glm::dvec3> outerRing = DiscretizeWireToRing3D(insetOuter, params.chordTolMm);
        if (outerRing.size() >= 3)
            rings.push_back(std::move(outerRing));
        return rings;
    }

    std::vector<TopoDS_Wire> holeWires;
    std::vector<std::vector<glm::dvec3>> holeRings;
    const bool traceHoles = StructureDebugTraceHoles();
    size_t holeSlot = 0;
    for (size_t i = 1; i < sourceWires.size(); ++i)
    {
        const std::vector<glm::dvec3> sourceRing =
            DiscretizeWireToRing3D(sourceWires[i].wire, params.chordTolMm);
        if (traceHoles && sourceRing.size() >= 3)
            LogHoleRingIdentity(holeSlot, i, sourceRing, planeNormal, "source-wire");

        const TopoDS_Wire outsetHole =
            OffsetHoleWireOutward(sourceWires[i].wire, params.insetMm, planeNormal, params.chordTolMm);
        if (outsetHole.IsNull())
        {
            if (traceHoles)
                std::cerr << "Structure hole trace sourceWire=" << i << " hole[" << holeSlot
                          << "]: OUTSET_OFFSET_FAILED\n";
            continue;
        }
        std::vector<glm::dvec3> holeRing = DiscretizeWireToRing3D(outsetHole, params.chordTolMm);
        if (holeRing.size() < 3)
        {
            if (traceHoles)
                std::cerr << "Structure hole trace sourceWire=" << i << " hole[" << holeSlot
                          << "]: OUTSET_RING_TOO_SMALL pts=" << holeRing.size() << "\n";
            continue;
        }
        if (traceHoles)
            LogHoleRingIdentity(holeSlot, i, holeRing, planeNormal, "outset-wire");
        holeWires.push_back(outsetHole);
        holeRings.push_back(std::move(holeRing));
        ++holeSlot;
    }

    if (holeRings.empty())
    {
        std::vector<glm::dvec3> outerRing = DiscretizeWireToRing3D(insetOuter, params.chordTolMm);
        if (outerRing.size() >= 3)
            rings.push_back(std::move(outerRing));
        return rings;
    }

    if (makeFaceOnlyTrim)
    {
        const std::vector<size_t> holeIndices = ResolvePreviewHoleIndices(holeWires, -1, -1);
        return ExtractRingsFromInsetOuterAndHoleWires(insetOuter, holeWires, holeIndices, planeNormal,
                                                      params.chordTolMm);
    }

    return BuildTrimmedPreviewRingsFromFootprintFace(insetOuter, sourceWires, holeWires, holeRings, params,
                                                     planeNormal);
}

std::vector<std::vector<glm::dvec3>> BuildTrimmedXyProjectedPreviewRings(const Face *face,
                                                                          const TopoDS_Face &occtFace,
                                                                          const BakeParams &params,
                                                                          bool mapOntoCap)
{
    const FacePlaneFrame frame = MakeFacePlaneFrame(face);
    const gp_Trsf toFlat = MakeFaceLocalFlatTrsf(frame);

    size_t holeCount = 0;
    for (TopExp_Explorer exWire(occtFace, TopAbs_WIRE); exWire.More(); exWire.Next())
        ++holeCount;
    if (holeCount > 0)
        --holeCount;

    bool usedPolygonSource = false;
    TopoDS_Face localSource;
    if (holeCount <= 2)
        localSource = TransformOcctFaceToLocalFlat(occtFace, toFlat);
    else
    {
        localSource =
            BuildFacePlaneLocalFlatSourceFace(face, occtFace, frame, params.chordTolMm, params.minFeatureMm);
        usedPolygonSource = !localSource.IsNull();
    }
    if (localSource.IsNull())
    {
        localSource = TransformOcctFaceToLocalFlat(occtFace, toFlat);
        usedPolygonSource = false;
    }
    if (localSource.IsNull())
    {
        localSource =
            BuildFacePlaneLocalFlatSourceFace(face, occtFace, frame, params.chordTolMm, params.minFeatureMm);
        usedPolygonSource = !localSource.IsNull();
    }
    if (localSource.IsNull())
        return {};

    const std::vector<FacePlaneSourceWire> sourceWires =
        CollectSortedFacePlaneSourceWires(localSource, params, kWorldUpNormal);
    if (sourceWires.empty())
        return {};

    std::vector<std::vector<glm::dvec3>> rings =
        BuildTrimmedPreviewRingsFromSourceWires(sourceWires, params, kWorldUpNormal, false);

    MapXyTrimmedRingsToPreviewSpace(rings, face, frame, toFlat, usedPolygonSource, mapOntoCap);

    if (StructureDebugEnabled())
    {
        std::cerr << "Structure cut outline: xyProjected trim rings=" << rings.size()
                  << " loops=" << sourceWires.size() << " polygonSource=" << usedPolygonSource
                  << " holes=" << holeCount << " mapOntoCap=" << mapOntoCap << "\n";
    }
    return rings;
}

TopoDS_Shape BuildCarveCompoundFromFootprint(const TopoDS_Shape &footprint, double zBottom, double chordTolMm,
                                             double minSpanMm)
{
    std::vector<std::vector<glm::dvec3>> rings =
        CollectRingsFromShape(footprint, chordTolMm, zBottom, 0.0);
    if (rings.empty())
        return TopoDS_Shape();

    // Drop only true sliver rings before grouping; minSpanMm applies to carve groups below.
    constexpr double kSliverSpanMm = 0.25;
    rings.erase(std::remove_if(rings.begin(), rings.end(),
                               [&](const std::vector<glm::dvec3> &ring)
                               {
                                   return ring.size() < 3 || RingSpanXyMm(ring) < kSliverSpanMm;
                               }),
                rings.end());
    if (rings.empty())
        return TopoDS_Shape();

    std::vector<CarveRingGroup> groups;
    if (footprint.ShapeType() == TopAbs_FACE && rings.size() > 1)
    {
        CarveRingGroup group;
        ClassifyOuterAndHoles(rings, kWorldUpNormal, group.outer, group.holes);
        if (!group.outer.empty())
            groups.push_back(std::move(group));
    }
    else
    {
        groups = PartitionRingsIntoCarveGroups(rings, kWorldUpNormal);
    }
    if (groups.empty())
        return TopoDS_Shape();

    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    bool any = false;
    for (const CarveRingGroup &group : groups)
    {
        if (RingSpanXyMm(group.outer) < minSpanMm)
            continue;
        const TopoDS_Face face =
            MakeFlatFaceFromOuterAndHolesPreferCircles(group.outer, group.holes, zBottom, chordTolMm);
        if (face.IsNull())
            continue;
        builder.Add(compound, face);
        any = true;
    }
    return any ? compound : TopoDS_Shape();
}

// Returns the first intersection distance t > 0 along ray (from + t*dir) with the ring, or -1.
double RayRingFirstExitT(const glm::dvec2 &from, const glm::dvec2 &dir,
                          const std::vector<glm::dvec3> &ring)
{
    double tMin = -1.0;
    const size_t n = ring.size();
    for (size_t i = 0; i < n; ++i)
    {
        const glm::dvec2 a(ring[i].x, ring[i].y);
        const glm::dvec2 b(ring[(i + 1) % n].x, ring[(i + 1) % n].y);
        const glm::dvec2 edge = b - a;
        const glm::dvec2 toA = a - from;
        const double det = dir.y * edge.x - dir.x * edge.y;
        if (std::abs(det) < 1e-12)
            continue;
        const double t = (edge.x * toA.y - edge.y * toA.x) / det;
        const double s = (dir.x * toA.y - dir.y * toA.x) / det;
        if (t > 1e-6 && s >= -1e-6 && s <= 1.0 + 1e-6)
        {
            if (tMin < 0.0 || t < tMin)
                tMin = t;
        }
    }
    return tMin;
}

std::vector<glm::dvec3> LargestRingXy(const TopoDS_Shape &footprint, double chordTolMm)
{
    const std::vector<std::vector<glm::dvec3>> rings =
        ExtractOffsetRingsFromFootprint(footprint, chordTolMm, 0.0);
    if (rings.empty())
        return {};
    size_t best = 0;
    double bestArea = 0.0;
    for (size_t i = 0; i < rings.size(); ++i)
    {
        const double area = std::abs(SignedArea2D(rings[i], kWorldUpNormal));
        if (area > bestArea)
        {
            bestArea = area;
            best = i;
        }
    }
    return rings[best];
}

// Flat rectangle from `from` toward `toward`, with given width, at z=zPlane.
// Extends 2000 mm in the forward direction so it always reaches the footprint boundary.
TopoDS_Face MakeStrutRectFaceAtZ(const glm::dvec2 &from, const glm::dvec2 &toward,
                                   double widthMm, double zPlane)
{
    const glm::dvec2 delta = toward - from;
    const double dist = glm::length(delta);
    if (dist < 1e-9)
        return TopoDS_Face();
    const glm::dvec2 dir = delta / dist;
    const glm::dvec2 perp(-dir.y, dir.x);
    const double hw = widthMm * 0.5;
    constexpr double kExtend = 2000.0;
    const glm::dvec2 a = from - dir * hw + perp * hw;
    const glm::dvec2 b = from - dir * hw - perp * hw;
    const glm::dvec2 c = from + dir * kExtend - perp * hw;
    const glm::dvec2 d = from + dir * kExtend + perp * hw;
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(gp_Pnt(a.x, a.y, zPlane));
    poly.Add(gp_Pnt(b.x, b.y, zPlane));
    poly.Add(gp_Pnt(c.x, c.y, zPlane));
    poly.Add(gp_Pnt(d.x, d.y, zPlane));
    poly.Close();
    if (!poly.IsDone())
        return TopoDS_Face();
    BRepBuilderAPI_MakeFace mkFace(poly.Wire());
    return mkFace.IsDone() ? mkFace.Face() : TopoDS_Face();
}

// Ray-cast point-in-polygon test (2D). Returns true if p is strictly inside verts.
bool PointInsidePolygon2D(const std::vector<glm::dvec2> &verts, const glm::dvec2 &p)
{
    const int n = static_cast<int>(verts.size());
    int crossings = 0;
    for (int i = 0; i < n; ++i)
    {
        const glm::dvec2 a = verts[i];
        const glm::dvec2 b = verts[(i + 1) % n];
        if ((a.y <= p.y && b.y > p.y) || (b.y <= p.y && a.y > p.y))
        {
            const double xIntersect = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
            if (p.x < xIntersect)
                ++crossings;
        }
    }
    return (crossings % 2) == 1;
}

// Returns true if segment AB properly crosses an edge of `ring` (a crossing strictly inside
// both segments — touches at shared endpoints/vertices, where anchors sit, don't count).
bool SegmentCrossesRing(const glm::dvec2 &a, const glm::dvec2 &b,
                        const std::vector<glm::dvec2> &ring)
{
    constexpr double kEps = 1e-6;
    for (int i = 0, n = static_cast<int>(ring.size()); i < n; ++i)
    {
        const glm::dvec2 p = ring[i];
        const glm::dvec2 q = ring[(i + 1) % n];
        const glm::dvec2 r = b - a;
        const glm::dvec2 d = q - p;
        const double rxd = r.x * d.y - r.y * d.x;
        if (std::abs(rxd) < 1e-12) continue;
        const glm::dvec2 diff = p - a;
        const double t = (diff.x * d.y - diff.y * d.x) / rxd;
        const double u = (diff.x * r.y - diff.y * r.x) / rxd;
        if (t > kEps && t < 1.0 - kEps && u > kEps && u < 1.0 - kEps)
            return true;
    }
    return false;
}

// A strut is valid only if it stays entirely within the carve region: it must not cross
// any ring boundary (outer wall or hole), its midpoint must land inside the outer ring
// (allRings.front() — rings are sorted largest-area-first by the caller), and its midpoint
// must NOT land inside any hole ring.
//
// Anchors sit exactly on the outer wire, so endpoints can't be tested for inside/outside —
// but a chord between two boundary points can run entirely outside the shape (e.g. across a
// concave notch) without "properly" crossing any edge, hence the midpoint check.
//
// The explicit "outside every hole" check matters for anchors that sit ON a hole's own
// boundary (hole-to-hole/hole-to-same-hole struts): a chord between two points on the SAME
// convex hole can run entirely through that hole's void without crossing its ring anywhere
// except at the two endpoints (which the crossing test deliberately excludes) — the crossing
// test alone cannot tell that case apart from a chord that legitimately detours through solid
// material around a concave hole's notch (also zero crossings). Checking hole-containment
// directly resolves the ambiguity. For struts starting on the outer wire this check is
// redundant (reaching a hole's interior from there requires crossing that hole's ring first,
// which the crossing test already catches) but harmless.
bool StrutIsValid(const glm::dvec2 &a, const glm::dvec2 &b,
                  const std::vector<std::vector<glm::dvec2>> &allRings,
                  int *dbgReason = nullptr)
{
    if (allRings.empty())
        return false;
    if (SegmentCrossesRing(a, b, allRings.front()))
    {
        if (dbgReason) *dbgReason = 0; // crosses outer
        return false;
    }
    for (size_t i = 1; i < allRings.size(); ++i)
        if (SegmentCrossesRing(a, b, allRings[i]))
        {
            if (dbgReason) *dbgReason = 1; // crosses a hole
            return false;
        }
    const glm::dvec2 mid = (a + b) * 0.5;
    if (!PointInsidePolygon2D(allRings.front(), mid))
    {
        if (dbgReason) *dbgReason = 2; // midpoint outside outer
        return false;
    }
    for (size_t i = 1; i < allRings.size(); ++i)
        if (PointInsidePolygon2D(allRings[i], mid))
        {
            if (dbgReason) *dbgReason = 3; // midpoint inside a hole
            return false;
        }
    return true;
}

} // namespace

std::vector<std::vector<glm::dvec3>> BuildFacePlaneOffsetPreviewRings(const Face *face,
                                                                      const BakeParams &params)
{
    std::vector<std::vector<glm::dvec3>> rings;
    if (face == nullptr || params.insetMm <= 1e-6)
        return rings;

    const TopoDS_Face occtFace = ResolvePlanarOcctFace(face);
    if (occtFace.IsNull())
        return rings;

    glm::dvec3 basisNormal{0.0, 0.0, 1.0};
    if (face->surface != nullptr)
        basisNormal = face->surface->GetNormal();
    const double nLen = glm::length(basisNormal);
    if (nLen > 1e-12)
        basisNormal /= nLen;

    const std::vector<FacePlaneSourceWire> sourceWires =
        CollectSortedFacePlaneSourceWires(occtFace, params, basisNormal);
    if (sourceWires.empty())
        return rings;

    const TopoDS_Wire insetOuter =
        OffsetOuterWireInward(sourceWires.front().wire, params.insetMm, basisNormal, params.chordTolMm);
    if (insetOuter.IsNull())
        return rings;

    if (sourceWires.size() <= 1)
    {
        std::vector<glm::dvec3> outerRing = DiscretizeWireToRing3D(insetOuter, params.chordTolMm);
        if (outerRing.size() >= 3)
            rings.push_back(std::move(outerRing));
        return rings;
    }

    std::vector<std::vector<glm::dvec3>> holeRings;
    for (size_t i = 1; i < sourceWires.size(); ++i)
    {
        const TopoDS_Wire outsetHole =
            OffsetHoleWireOutward(sourceWires[i].wire, params.insetMm, basisNormal, params.chordTolMm);
        if (outsetHole.IsNull())
            continue;
        std::vector<glm::dvec3> holeRing = DiscretizeWireToRing3D(outsetHole, params.chordTolMm);
        if (holeRing.size() >= 3)
            holeRings.push_back(std::move(holeRing));
    }

    if (holeRings.empty())
    {
        std::vector<glm::dvec3> outerRing = DiscretizeWireToRing3D(insetOuter, params.chordTolMm);
        if (outerRing.size() >= 3)
            rings.push_back(std::move(outerRing));
        return rings;
    }

    std::vector<glm::dvec3> outerRing = DiscretizeWireToRing3D(insetOuter, params.chordTolMm);
    if (outerRing.size() >= 3)
        rings.push_back(std::move(outerRing));
    for (const std::vector<glm::dvec3> &holeRing : holeRings)
    {
        if (holeRing.size() >= 3)
            rings.push_back(holeRing);
    }

    const char *debugEnv = std::getenv("CAD_DEBUG_STRUCTURE");
    if (debugEnv != nullptr && debugEnv[0] != '\0' && !(debugEnv[0] == '0' && debugEnv[1] == '\0'))
    {
        const bool holeOutsetsOverlap = holeRings.size() > 1 && HoleOutsetRingsOverlap(holeRings, basisNormal);
        std::cerr << "Structure preview rings: sourceLoops=" << sourceWires.size() << " previewRings="
                  << rings.size() << " holeRings=" << holeRings.size() << " overlap=" << holeOutsetsOverlap
                  << "\n";
    }

    return rings;
}

std::vector<std::vector<glm::dvec3>> BuildTrimmedFacePlanePreviewRings(const Face *face,
                                                                       const BakeParams &params)
{
    std::vector<std::vector<glm::dvec3>> rings;
    if (face == nullptr || params.insetMm <= 1e-6)
        return rings;

    const TopoDS_Face occtFace = ResolvePlanarOcctFace(face);
    if (occtFace.IsNull())
        return rings;

    glm::dvec3 basisNormal{0.0, 0.0, 1.0};
    if (face->surface != nullptr)
        basisNormal = face->surface->GetNormal();
    const double nLen = glm::length(basisNormal);
    if (nLen > 1e-12)
        basisNormal /= nLen;

    const std::vector<FacePlaneSourceWire> sourceWires =
        CollectSortedFacePlaneSourceWires(occtFace, params, basisNormal);
    if (sourceWires.empty())
        return rings;

    constexpr double kHorizontalOffsetThreshold = 0.995;
    /// Local-flat trim for many holes on ~0–16° tilt (avoids oblique boolean hangs). Few holes use the
    /// same face-plane offset+trim as `BuildFacePlaneOffsetPreviewRings` so trim matches raw offsets.
    constexpr double kXyProjectedTrimMinNormalZ = 0.96;
    const double nz = std::abs(basisNormal.z);
    const size_t holeCount = sourceWires.size() - 1;
    if (nz < kHorizontalOffsetThreshold && nz >= kXyProjectedTrimMinNormalZ && holeCount > 2)
        return BuildTrimmedXyProjectedPreviewRings(face, occtFace, params, true);

    return BuildTrimmedPreviewRingsFromSourceWires(sourceWires, params, basisNormal);
}

TopoDS_Shape BuildOffsetFootprintOnFace(const Face *face, const BakeParams &params)
{
    if (face == nullptr || face->loops.empty())
        return TopoDS_Shape();

    const TopoDS_Face occtFace = ResolvePlanarOcctFace(face);
    if (occtFace.IsNull())
        return TopoDS_Shape();

    if (params.insetMm <= 1e-6)
    {
        glm::dvec3 n{0.0, 0.0, 1.0};
        if (face->surface != nullptr)
            n = face->surface->GetNormal();
        const double nLen = glm::length(n);
        const double nz = nLen > 1e-12 ? std::abs(n.z / nLen) : 0.0;
        constexpr double kHorizontalOffsetThreshold = 0.995;
        if (nz >= kHorizontalOffsetThreshold)
            return occtFace;
        return BuildXyProjectedSourceFace(face, occtFace, kXyPlaneZ, params.chordTolMm, params.minFeatureMm);
    }

    glm::dvec3 n{0.0, 0.0, 1.0};
    if (face->surface != nullptr)
        n = face->surface->GetNormal();
    const double nLen = glm::length(n);
    const double nz = nLen > 1e-12 ? std::abs(n.z / nLen) : 0.0;
    const bool hasHoles = face->loops.size() > 1;

    constexpr double kHorizontalOffsetThreshold = 0.995;
    if (nz >= kHorizontalOffsetThreshold)
    {
        if (hasHoles)
        {
            // BRepOffsetAPI_MakeOffset on a face moves all wires in the same signed direction,
            // which shrinks holes rather than expanding them.  Use the XY-holed path so hole
            // boundaries are correctly outset by insetMm.
            const TopoDS_Shape xyHoled = BuildXyHoledOffsetFootprint(face, occtFace, params);
            if (!xyHoled.IsNull())
                return xyHoled;
        }
        BRepOffsetAPI_MakeOffset offsetMaker;
        offsetMaker.Init(occtFace, GeomAbs_Arc);
        offsetMaker.Perform(-params.insetMm);
        if (!offsetMaker.IsDone())
            return TopoDS_Shape();
        return offsetMaker.Shape();
    }

    if (hasHoles)
    {
        const TopoDS_Shape xyHoled = BuildXyHoledOffsetFootprint(face, occtFace, params);
        if (!xyHoled.IsNull())
            return xyHoled;

        BRepOffsetAPI_MakeOffset offsetMaker;
        offsetMaker.Init(occtFace, GeomAbs_Arc);
        offsetMaker.Perform(-params.insetMm);
        if (!offsetMaker.IsDone())
            return TopoDS_Shape();
        return offsetMaker.Shape();
    }

    const TopoDS_Face xySource =
        BuildXyProjectedSourceFace(face, occtFace, kXyPlaneZ, params.chordTolMm, params.minFeatureMm);
    if (xySource.IsNull())
        return TopoDS_Shape();

    BRepOffsetAPI_MakeOffset offsetMaker;
    offsetMaker.Init(xySource, GeomAbs_Arc);
    offsetMaker.Perform(-params.insetMm);
    if (!offsetMaker.IsDone())
        return TopoDS_Shape();
    return offsetMaker.Shape();
}

// --- Curve-preserving carve ---------------------------------------------------
// The curve-native carve path: build the inset/outset offset footprint as a real OCCT face with its
// ANALYTIC edges intact (lines, arcs, ellipses — no discretization), vertically project tilted faces
// to the cutting plane via an affine shear, notch the struts and round the corners (ChFi2d) on the
// curved geometry, then extrude. A circular hole carves a cylinder wall instead of a ~450-face
// polygon prism — fixing the freeze/OOM the old polygon path hit on real models. It is the DEFAULT;
// every stage falls back to the polygon path on failure. Set CAD_STRUCTURE_CURVED=0 to force the
// legacy polygon path (debug/escape hatch).

bool StructureCurvedCarveEnabled()
{
    const char *env = std::getenv("CAD_STRUCTURE_CURVED");
    // Default ON: only an explicit "0" disables it.
    return !(env != nullptr && env[0] == '0' && env[1] == '\0');
}


TopoDS_Shape BuildCurvedCarveFootprintMvp(const Face *face, const BakeParams &params)
{
    if (face == nullptr || params.insetMm <= 1e-6)
        return TopoDS_Shape();
    const TopoDS_Face occtFace = ResolvePlanarOcctFace(face);
    if (occtFace.IsNull())
        return TopoDS_Shape();

    // PROJECT FIRST, then offset. Vertically project the source face onto the horizontal cutting
    // plane (z=0) and do the inset/outset + trim THERE — so the offset is uniform in the projected
    // XY (identical to a flat face), instead of being applied on the tilted face and then
    // foreshortened by the projection (which made tilted holes grow less than their flat twins). The
    // projection is a non-singular affine shear (maps the face plane n·P=D to z=0, keeps XY), so a
    // tilted circle/ellipse becomes a true ellipse — analytic edges preserved, no discretization.
    const FacePlaneFrame frame = MakeFacePlaneFrame(face);
    glm::dvec3 n = frame.n;
    const double nLen = glm::length(n);
    if (nLen > 1e-9)
        n /= nLen;
    if (std::abs(n.z) < 1e-6) // too vertical to flatten — caller falls back to polygon
        return TopoDS_Shape();
    const double planeD = glm::dot(n, frame.origin);
    gp_GTrsf shear;
    shear.SetValue(1, 1, 1.0);        shear.SetValue(1, 2, 0.0);        shear.SetValue(1, 3, 0.0);  shear.SetValue(1, 4, 0.0);
    shear.SetValue(2, 1, 0.0);        shear.SetValue(2, 2, 1.0);        shear.SetValue(2, 3, 0.0);  shear.SetValue(2, 4, 0.0);
    shear.SetValue(3, 1, n.x / n.z);  shear.SetValue(3, 2, n.y / n.z);  shear.SetValue(3, 3, 1.0);  shear.SetValue(3, 4, -planeD / n.z);

    TopoDS_Shape xyShape;
    try
    {
        xyShape = BRepBuilderAPI_GTransform(occtFace, shear, Standard_True).Shape();
    }
    catch (const Standard_Failure &)
    {
        return TopoDS_Shape();
    }
    // Re-plane onto an explicit z=0 Geom_Plane (GTransform leaves a non-canonical planar surface).
    xyShape = ReplanarizeFootprintToXy(xyShape);
    if (xyShape.IsNull())
        return TopoDS_Shape();
    TopoDS_Face xySource;
    {
        TopExp_Explorer fexp(xyShape, TopAbs_FACE);
        if (!fexp.More())
            return TopoDS_Shape();
        xySource = TopoDS::Face(fexp.Current());
    }

    const std::vector<FacePlaneSourceWire> sourceWires =
        CollectSortedFacePlaneSourceWires(xySource, params, kWorldUpNormal);
    if (sourceWires.empty())
        return TopoDS_Shape();

    const TopoDS_Wire insetOuter =
        OffsetOuterWireInward(sourceWires.front().wire, params.insetMm, kWorldUpNormal, params.chordTolMm);
    if (insetOuter.IsNull())
        return TopoDS_Shape();

    std::vector<TopoDS_Wire> holeWires;
    for (size_t i = 1; i < sourceWires.size(); ++i)
    {
        const TopoDS_Wire outset =
            OffsetHoleWireOutward(sourceWires[i].wire, params.insetMm, kWorldUpNormal, params.chordTolMm);
        if (!outset.IsNull())
            holeWires.push_back(outset);
    }
    std::vector<size_t> holeIndices(holeWires.size());
    for (size_t i = 0; i < holeWires.size(); ++i)
        holeIndices[i] = i;

    // TryBooleanTrimPreview cuts the holes with the explicit-plane MakeFace (curve-preserving) and
    // hands back the curved footprint as resultShape — already in world XY since we projected first.
    const BooleanTrimOutcome outcome =
        TryBooleanTrimPreview(insetOuter, holeWires, holeIndices, params, kWorldUpNormal);
    if (outcome.resultShape.IsNull())
        return TopoDS_Shape();

    // Restore a clean Geom_Plane for the native fillet downstream.
    TopoDS_Shape worldShape = ReplanarizeFootprintToXy(outcome.resultShape);
    if (worldShape.IsNull() || !ValidateOutlineShapeTopology(worldShape, "BuildCurvedCarveFootprintMvp"))
        return TopoDS_Shape();
    return worldShape;
}

TopoDS_Shape ExtrudeCurvedFootprintForCarve(const TopoDS_Shape &footprint, double zBottom, double zTop)
{
    if (footprint.IsNull() || !(zBottom < zTop))
        return TopoDS_Shape();

    // The curved footprint is a horizontal planar face at z≈0; drop it to zBottom and extrude up so
    // the prism spans the solid for a through-cut (matching the polygon path's place-at-zBottom).
    Bnd_Box box;
    BRepBndLib::Add(footprint, box);
    if (box.IsVoid())
        return TopoDS_Shape();
    double xmin, ymin, zmin, xmax, ymax, zmax;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    gp_Trsf trsf;
    trsf.SetTranslation(gp_Vec(0.0, 0.0, zBottom - zmin));
    const TopoDS_Shape placed = BRepBuilderAPI_Transform(footprint, trsf, Standard_True).Shape();
    if (placed.IsNull())
        return TopoDS_Shape();

    BRepPrimAPI_MakePrism prismMaker(placed, gp_Vec(0.0, 0.0, zTop - zBottom));
    const TopoDS_Shape prism = prismMaker.Shape();
    if (prism.IsNull() || !ValidateOutlineShapeTopology(prism, "ExtrudeCurvedFootprintForCarve"))
        return TopoDS_Shape();
    return prism;
}

std::vector<std::vector<glm::dvec3>> ExtractOffsetRingsFromFootprint(const TopoDS_Shape &footprint,
                                                                     double chordTolMm, double zPlane)
{
    std::vector<std::vector<glm::dvec3>> rings =
        CollectRingsFromShape(footprint, chordTolMm, zPlane, 0.0);
    constexpr double kSliverSpanMm = 0.25;
    rings.erase(std::remove_if(rings.begin(), rings.end(),
                               [&](const std::vector<glm::dvec3> &ring)
                               {
                                   return ring.size() < 3 || RingSpanXyMm(ring) < kSliverSpanMm;
                               }),
                rings.end());
    return rings;
}

std::vector<std::vector<glm::dvec3>> ExtractPreviewRingsFromFootprint(const TopoDS_Shape &footprint,
                                                                       double chordTolMm,
                                                                       double minSpanMm)
{
    if (minSpanMm <= 0.0)
        return ExtractOffsetRingsFromFootprint(footprint, chordTolMm, 0.0);

    std::vector<std::vector<glm::dvec3>> rings = ExtractOffsetRingsFromFootprint(footprint, chordTolMm, 0.0);
    rings.erase(std::remove_if(rings.begin(), rings.end(),
                               [&](const std::vector<glm::dvec3> &ring)
                               { return RingSpanXyMm(ring) < minSpanMm; }),
                rings.end());
    return rings;
}

/// XY carve footprint preview: flatten face → offset in horizontal working plane → boolean trim → map to world on the cap.
std::vector<std::vector<glm::dvec3>> BuildXyCarveFootprintPreviewRings(const Face *face,
                                                                       const BakeParams &params)
{
    if (face == nullptr || params.insetMm <= 1e-6)
        return {};
    const TopoDS_Face occtFace = ResolvePlanarOcctFace(face);
    if (occtFace.IsNull())
        return {};
    return BuildTrimmedXyProjectedPreviewRings(face, occtFace, params,
                                               !StructurePreviewDrawsCarveFootprintInWorldXy());
}

// Shared setup for OCCT-based anchor generation: coordinate transforms and outer wire.
// Returns false if setup fails.
struct StrutSetup
{
    TopoDS_Wire                       outerWire;
    std::vector<TopoDS_Wire>          holeWires;      // outset hole wires, same local-flat frame as outerWire
    TopoDS_Wire                       rawInsetOuter;  // raw inset outer (pre-boolean) for provenance filtering
    std::vector<FacePlaneSourceWire>  sourceWires;    // pre-inset source wires (face boundary before offset)
    float                             zPlane     = 0.0f;
    gp_Trsf                           toFlatInv;
};

bool BuildStrutSetup(const Face *face, const BakeParams &params, StrutSetup &out)
{
    // z-plane from preview rings (world XY z coordinate)
    const std::vector<std::vector<glm::dvec3>> rings =
        StructurePreviewUsesXyCarveFootprint() ? BuildXyCarveFootprintPreviewRings(face, params)
                                               : BuildTrimmedFacePlanePreviewRings(face, params);

    glm::dvec3 basisNormal{0.0, 0.0, 1.0};
    if (face->surface != nullptr)
        basisNormal = face->surface->GetNormal();
    const double nLen = glm::length(basisNormal);
    if (nLen > 1e-12)
        basisNormal /= nLen;
    glm::dvec3 lift{0.0, 0.0, 0.08};
    if (!StructurePreviewDrawsCarveFootprintInWorldXy() && nLen > 1e-12)
        lift = basisNormal * 0.08;

    if (!rings.empty() && !rings.front().empty())
    {
        double zSum = 0.0;
        for (const glm::dvec3 &p : rings.front())
            zSum += p.z;
        out.zPlane = static_cast<float>(zSum / static_cast<double>(rings.front().size()) + lift.z);
    }

    // Build inset wire from the same local-flat source as preview rings, using the same
    // OffsetOuterWireInward method.  This guarantees anchor midpoints land exactly on the
    // drawn outline regardless of face tilt or edge type (arcs, lines).
    //
    // The face analysis (OCCT face, frame, transforms, source wires) is inset-independent and
    // cached per (face, chordTolMm, minFeatureMm) so repeated calls with different insets only
    // rerun the fast offset steps below.
    const PreOffsetCacheKey preKey{face, params.chordTolMm, params.minFeatureMm};
    auto &preCache = PreOffsetCache();
    if (auto it = preCache.find(preKey); it == preCache.end())
    {
        const TopoDS_Face occtFace = ResolvePlanarOcctFace(face);
        if (occtFace.IsNull())
            return false;

        const FacePlaneFrame frame = MakeFacePlaneFrame(face);
        const gp_Trsf toFlat = MakeFaceLocalFlatTrsf(frame);

        TopoDS_Face localSource = TransformOcctFaceToLocalFlat(occtFace, toFlat);
        if (localSource.IsNull())
            localSource = BuildFacePlaneLocalFlatSourceFace(face, occtFace, frame,
                                                             params.chordTolMm, params.minFeatureMm);
        if (localSource.IsNull())
            return false;

        auto wires = CollectSortedFacePlaneSourceWires(localSource, params, kWorldUpNormal);
        if (wires.empty())
            return false;

        PreOffsetData preData;
        preData.sourceWires = std::move(wires);
        preData.toFlatInv   = toFlat.Inverted();
        preCache.emplace(preKey, std::move(preData));
    }
    const PreOffsetData &pre = preCache.at(preKey);
    out.toFlatInv = pre.toFlatInv;
    out.sourceWires = pre.sourceWires;
    const std::vector<FacePlaneSourceWire> &sourceWires = pre.sourceWires;
    if (sourceWires.empty())
        return false;

    // Inset the outer wire inward — same as the preview ring offset
    const TopoDS_Wire insetOuter =
        OffsetOuterWireInward(sourceWires.front().wire, params.insetMm, kWorldUpNormal, params.chordTolMm);
    if (insetOuter.IsNull())
        return false;
    out.rawInsetOuter = insetOuter;

    if (sourceWires.size() <= 1)
    {
        // No holes: use raw inset outer wire directly
        out.outerWire = insetOuter;
    }
    else
    {
        // Has holes: run the same boolean trim used by preview rings so the result wire
        // naturally splits edges clipped by hole outsets (e.g. hypotenuse cut in two).
        std::vector<TopoDS_Wire> holeWires;
        for (size_t i = 1; i < sourceWires.size(); ++i)
        {
            const TopoDS_Wire outsetHole = OffsetHoleWireOutward(
                sourceWires[i].wire, params.insetMm, kWorldUpNormal, params.chordTolMm);
            if (!outsetHole.IsNull())
                holeWires.push_back(outsetHole);
        }

        TopoDS_Wire resultWire;
        std::vector<TopoDS_Wire> resultInnerWires;
        if (!holeWires.empty())
        {
            const std::vector<size_t> holeIndices = ResolvePreviewHoleIndices(holeWires, -1, -1);
            const BooleanTrimOutcome boolOutcome =
                TryBooleanTrimPreview(insetOuter, holeWires, holeIndices, params, kWorldUpNormal);
            // Extract the largest-area result face's outer wire AND its own remaining inner
            // wires (if any) — NOT the raw pre-trim hole-outset wires. A hole close enough to
            // the boundary that subtracting it merges into a notch (rather than leaving a
            // separate enclosed island) makes the trim absorb that whole boundary into the
            // result face's own outer wire, with no inner wire left for it; reusing the stale
            // pre-trim hole wire in that case re-introduces the same boundary a second time as
            // a spurious "hole", duplicating anchors there and carving the valid region down to
            // nothing along it.
            if (!boolOutcome.resultShape.IsNull())
            {
                double bestArea = -1.0;
                TopExp_Explorer faceExp(boolOutcome.resultShape, TopAbs_FACE);
                for (; faceExp.More(); faceExp.Next())
                {
                    const TopoDS_Face &rf = TopoDS::Face(faceExp.Current());
                    GProp_GProps props;
                    BRepGProp::SurfaceProperties(rf, props);
                    const double area = std::abs(props.Mass());
                    if (area > bestArea)
                    {
                        bestArea = area;
                        resultWire = BRepTools::OuterWire(rf);
                        resultInnerWires.clear();
                        for (TopExp_Explorer wireExp(rf, TopAbs_WIRE); wireExp.More(); wireExp.Next())
                        {
                            const TopoDS_Wire w = TopoDS::Wire(wireExp.Current());
                            if (!w.IsSame(resultWire))
                                resultInnerWires.push_back(w);
                        }
                    }
                }
            }
        }
        out.outerWire = !resultWire.IsNull() ? resultWire : insetOuter;
        out.holeWires = !resultWire.IsNull() ? std::move(resultInnerWires) : std::move(holeWires);
    }
    if (out.outerWire.IsNull())
        return false;

    return true;
}

// TODO(anchors): the previous edge-skip heuristics (min-length sliver filter,
// fillet-radius==insetMm filter, and "hole polygon" chord-run consolidation via
// collinearity against rawInsetOuter) were removed because they were fragile
// proxies for two different problems. When anchor selection is revisited:
//  1. Replace the sliver/fillet filters with a proper anchor-selection algorithm
//     driven by OCCT offset provenance (BRepBuilderAPI_MakeShape::Generated/
//     Modified/IsDeleted), which can definitively identify edges introduced by
//     the inward-offset corner rounding rather than guessing from length/radius.
//  2. Detect and convert polygon-approximated arcs (from STL/OBJ imports, where
//     curved hole boundaries arrive as chains of short straight chords) into true
//     OCCT analytic arcs at import time, so isCircle-based detection works
//     uniformly and the "hole polygon" special-casing becomes unnecessary.
// Builds the wall anchors used both for the anchor-point preview overlay and for
// strut-line generation, so the two stay in lockstep. One anchor is placed per
// logical curve along s.outerWire — see the tangent-continuity merge below for
// why a naive one-anchor-per-OCCT-edge walk over-counts on some faces.
std::vector<StrutAnchor> ComputeStrutWallAnchors(const StrutSetup &s, const BakeParams &params,
                                                 size_t *outerAnchorCount = nullptr,
                                                 std::vector<std::vector<glm::dvec2>> *outAnchorWallPts = nullptr)
{
    struct EdgeSpan
    {
        std::vector<glm::dvec2> pts;
        glm::dvec2              startTangent{0.0};
        glm::dvec2              endTangent{0.0};
        double                  startCurvature = 0.0;
        double                  endCurvature   = 0.0;
    };

    constexpr double kTangentContinuityCos = 0.999;
    constexpr double kCurvatureAbsTol      = 1e-4;
    constexpr double kCurvatureRelTol      = 0.02;

    auto isContinuous = [&](const glm::dvec2 &endTan, const glm::dvec2 &startTan,
                            double endKappa, double startKappa) -> bool
    {
        if (glm::length(endTan) <= 1e-12 || glm::length(startTan) <= 1e-12 ||
            glm::dot(endTan, startTan) < kTangentContinuityCos)
            return false;
        const double diff  = std::abs(endKappa - startKappa);
        const double scale = std::max({std::abs(endKappa), std::abs(startKappa), kCurvatureAbsTol});
        return diff <= std::max(kCurvatureAbsTol, kCurvatureRelTol * scale);
    };

    // Parallel to the flattened `result` anchor list below: each anchor's own wall-run polyline
    // (the same `run` it was placed on), in the local-flat 2D frame — used downstream to clip
    // each strut end against its own wall's real geometry instead of a flat tangent-line miter.
    std::vector<std::vector<glm::dvec2>> allRuns;

    // Walk one wire and return the anchors it contributes. `isHole` selects which side of
    // the wire's own orientation is "inward" — see the signed-area block below.
    auto anchorsForWire = [&](const TopoDS_Wire &wire, bool isHole) -> std::vector<StrutAnchor>
    {
        std::vector<EdgeSpan> spans;
        for (BRepTools_WireExplorer edgeExp(wire); edgeExp.More(); edgeExp.Next())
        {
            const TopoDS_Edge &occtEdge = edgeExp.Current();
            BRepAdaptor_Curve adaptor(occtEdge);
            const Standard_Real first = adaptor.FirstParameter();
            const Standard_Real last  = adaptor.LastParameter();

            auto tangentAt = [&](Standard_Real param) -> glm::dvec2
            {
                const gp_Vec tv = adaptor.DN(param, 1).Transformed(s.toFlatInv);
                const glm::dvec2 t(tv.X(), tv.Y());
                const double len = glm::length(t);
                return len > 1e-12 ? t / len : glm::dvec2(0.0);
            };
            // Curvature is rigid-transform invariant, so compute it directly from
            // the curve's own derivatives (no need to go through toFlatInv). It is
            // what tells a smooth wall->fillet transition (kappa jumps 0 -> 1/R)
            // apart from a single arc that OCCT happened to split into two edges
            // (kappa stays constant across the seam).
            auto curvatureAt = [&](Standard_Real param) -> double
            {
                gp_Pnt p;
                gp_Vec d1, d2;
                adaptor.D2(param, p, d1, d2);
                const double d1Len = d1.Magnitude();
                if (d1Len < 1e-12)
                    return 0.0;
                return d1.Crossed(d2).Magnitude() / (d1Len * d1Len * d1Len);
            };

            std::vector<glm::dvec3> pts3D;
            CollectPointsFromEdge(occtEdge, params.chordTolMm, pts3D);
            if (pts3D.size() < 2)
                continue;

            EdgeSpan span;
            span.pts.reserve(pts3D.size());
            for (const glm::dvec3 &p3 : pts3D)
            {
                const gp_Pnt p = gp_Pnt(p3.x, p3.y, p3.z).Transformed(s.toFlatInv);
                span.pts.push_back({p.X(), p.Y()});
            }
            span.startTangent   = tangentAt(first);
            span.endTangent     = tangentAt(last);
            span.startCurvature = curvatureAt(first);
            span.endCurvature   = curvatureAt(last);
            spans.push_back(std::move(span));
        }
        if (spans.empty())
            return {};

        // Wire's own signed area (shoelace formula over every sampled point, in order) gives
        // this wire's actual winding direction, independent of any face-wide reference point.
        // A single global centroid (the previous approach) picks the wrong inward side on
        // concave walls or off-center holes; orientation derived from the wire's own traversal
        // is correct for any simple closed curve, convex or not.
        double signedArea = 0.0;
        {
            glm::dvec2 first{0.0, 0.0};
            glm::dvec2 prev{0.0, 0.0};
            bool havePrev = false;
            for (const EdgeSpan &sp : spans)
            {
                for (const glm::dvec2 &p : sp.pts)
                {
                    if (havePrev)
                        signedArea += prev.x * p.y - p.x * prev.y;
                    else
                        first = p;
                    prev = p;
                    havePrev = true;
                }
            }
            if (havePrev)
                signedArea += prev.x * first.y - first.x * prev.y;
        }
        // For the outer wire, "inward" is into this wire's own enclosed region (the standard
        // left-of-travel convention). For a hole wire, "inward" (into the surrounding solid)
        // is the OPPOSITE side, since the wire's own enclosed region is the void it carves out.
        const double windingSign  = (signedArea >= 0.0) ? 1.0 : -1.0;
        const double interiorSign = isHole ? -windingSign : windingSign;

        // Merge runs of tangent- AND curvature-continuous edges into a single curve
        // before placing anchors. OCCT frequently splits one logical curve into
        // multiple edges at a seam — e.g. a circular fillet on a slanted face stored
        // as two half-ellipse arcs (see Structure ellipse/B-spline hole notes) —
        // which would otherwise produce one duplicate anchor per split, even though
        // the XY-aligned version of the same face keeps it as a single edge.
        // Tangent continuity alone isn't enough: a straight wall is tangent to the
        // fillet arc at their shared corner (that's what makes it a smooth fillet),
        // but they are still two distinct walls that each need their own anchor —
        // curvature jumps from 0 to 1/R there, while a true same-curve split keeps
        // curvature constant across the seam.
        std::vector<std::vector<glm::dvec2>> runs;
        for (size_t i = 0; i < spans.size(); ++i)
        {
            if (i > 0 && isContinuous(spans[i - 1].endTangent, spans[i].startTangent,
                                      spans[i - 1].endCurvature, spans[i].startCurvature))
            {
                std::vector<glm::dvec2> &run = runs.back();
                for (size_t k = 1; k < spans[i].pts.size(); ++k)
                    run.push_back(spans[i].pts[k]);
            }
            else
            {
                runs.push_back(spans[i].pts);
            }
        }
        // The wire is a closed loop — if the last run flows smoothly into the first,
        // they are the same curve split only by where the wire happens to start.
        if (runs.size() > 1 && isContinuous(spans.back().endTangent, spans.front().startTangent,
                                            spans.back().endCurvature, spans.front().startCurvature))
        {
            std::vector<glm::dvec2> &firstRun = runs.front();
            std::vector<glm::dvec2> &lastRun  = runs.back();
            for (size_t k = 1; k < firstRun.size(); ++k)
                lastRun.push_back(firstRun[k]);
            runs.erase(runs.begin());
        }

        // Degenerate slivers (near-zero-length runs that survived the tangent/curvature merge
        // above only because they're NOT actually continuous with their neighbor) can't yield a
        // trustworthy local tangent of their own — confirmed case: a ~0.08mm residual at a corner
        // (from a source fillet whose radius is close to the inset distance, so insetting shrinks
        // it toward a sharp point and OCCT's offset leaves a tiny numerically-noisy edge instead of
        // cleanly collapsing to one point) produced a backward-pointing normal there, silently
        // breaking a real, valid strut elsewhere on that wall. Rather than dropping them (which
        // leaves a real gap — no anchor at all near that corner for a strut to land on) or merging
        // by array index (unsafe: `runs` order doesn't reliably mirror wire adjacency, since the
        // wire's arbitrary start vertex can split one corner across array positions that aren't
        // next to each other), each sliver's normal is instead derived from the bisector of its two
        // true geometric neighbors — found by matching shared endpoint coordinates, not array
        // position — using their already-correct normals.
        constexpr double kMinRunLenMm = 0.5;
        std::vector<std::vector<glm::dvec2>> goodRuns;
        std::vector<std::vector<glm::dvec2>> sliverRuns;
        for (auto &run : runs)
        {
            double len = 0.0;
            for (size_t k = 0; k + 1 < run.size(); ++k)
                len += glm::distance(run[k], run[k + 1]);
            (len < kMinRunLenMm ? sliverRuns : goodRuns).push_back(std::move(run));
        }

        // Arc-length midpoint + tangent-derived normal for one run. Shared by both good runs and
        // (for position only) slivers.
        auto placeOnRun = [&](const std::vector<glm::dvec2> &run, glm::dvec2 &outMid2D,
                              glm::dvec2 &outN, bool &outOk)
        {
            outOk = false;
            if (run.size() < 2)
                return;
            std::vector<double> segLens(run.size() - 1);
            double totalLen = 0.0;
            for (size_t i = 0; i + 1 < run.size(); ++i)
            {
                segLens[i] = glm::distance(run[i], run[i + 1]);
                totalLen += segLens[i];
            }
            if (totalLen < 1e-12)
                return;

            // Anchor sits at the curve's arc-length midpoint so it lands on the
            // outline regardless of how many edges the run was assembled from.
            const double half = totalLen * 0.5;
            double acc = 0.0;
            glm::dvec2 mid2D = run.front();
            glm::dvec2 D{1.0, 0.0};
            for (size_t i = 0; i + 1 < run.size(); ++i)
            {
                if (acc + segLens[i] >= half || i + 2 == run.size())
                {
                    const double t = segLens[i] > 1e-12 ? (half - acc) / segLens[i] : 0.0;
                    mid2D = glm::mix(run[i], run[i + 1], glm::clamp(t, 0.0, 1.0));
                    const glm::dvec2 dir = run[i + 1] - run[i];
                    const double dirLen = glm::length(dir);
                    if (dirLen > 1e-12)
                        D = dir / dirLen;
                    break;
                }
                acc += segLens[i];
            }

            const glm::dvec2 nCand(-D.y, D.x);
            outMid2D = mid2D;
            outN = (interiorSign >= 0.0) ? nCand : -nCand;
            outOk = true;
        };

        std::vector<StrutAnchor> anchors;
        anchors.reserve(goodRuns.size() + sliverRuns.size());
        // (endpointA, endpointB, N) per good run, for sliver-neighbor matching by coordinate.
        struct GoodRunEnds { glm::dvec2 a, b; glm::dvec2 N; };
        std::vector<GoodRunEnds> goodEnds;
        goodEnds.reserve(goodRuns.size());
        for (const auto &run : goodRuns)
        {
            glm::dvec2 mid2D, N;
            bool ok = false;
            placeOnRun(run, mid2D, N, ok);
            if (!ok)
                continue;
            anchors.push_back({glm::vec3(mid2D.x, mid2D.y, s.zPlane), glm::vec2(N.x, N.y)});
            allRuns.push_back(run);
            goodEnds.push_back({run.front(), run.back(), N});
        }

        constexpr double kEndpointMatchTolMm = 1e-3;
        for (const auto &run : sliverRuns)
        {
            glm::dvec2 mid2D, unusedN;
            bool ok = false;
            placeOnRun(run, mid2D, unusedN, ok);
            if (!ok)
                continue;

            glm::dvec2 nSum(0.0);
            int nMatches = 0;
            for (const glm::dvec2 &sliverEnd : {run.front(), run.back()})
            {
                for (const GoodRunEnds &g : goodEnds)
                {
                    if (glm::distance(sliverEnd, g.a) < kEndpointMatchTolMm ||
                        glm::distance(sliverEnd, g.b) < kEndpointMatchTolMm)
                    {
                        nSum += g.N;
                        ++nMatches;
                        break;
                    }
                }
            }
            if (nMatches == 0)
                continue; // no reliable neighbor found — drop, same as before.
            const double nSumLen = glm::length(nSum);
            const glm::dvec2 N = nSumLen > 1e-9 ? nSum / nSumLen : glm::dvec2(0.0);
            anchors.push_back({glm::vec3(mid2D.x, mid2D.y, s.zPlane), glm::vec2(N.x, N.y)});
            // Don't hand the raw sub-mm run to BuildWallClipRegion: its own local tangent (the
            // only thing the region builder derives from the run's points) is numerically
            // unreliable at that scale — exactly the case the bisector-normal fix above worked
            // around for anchor placement, but BuildWallClipRegion recomputes a *fresh* per-point
            // tangent from the run it's given, ignoring the corrected N except to flip its sign.
            // A clean synthetic 2-point run, perpendicular to the already-correct bisector N and
            // centered on the anchor, gives the region builder a tangent that actually matches N
            // — long enough to avoid precision noise, short enough not to bleed into a
            // neighbouring wall's territory at a sharp corner.
            const glm::dvec2 tangent(-N.y, N.x);
            constexpr double kSliverSyntheticHalfLenMm = 1.0;
            allRuns.push_back({mid2D - tangent * kSliverSyntheticHalfLenMm,
                               mid2D + tangent * kSliverSyntheticHalfLenMm});
        }
        return anchors;
    };

    // Outer wire anchors + hole wire anchors. Hole wires are the inset-expanded hole
    // boundaries — at small insets they may not intersect the outer wire (so the outer
    // wire's edge count stays low), but their walls still need anchors for strut generation.
    std::vector<StrutAnchor> result = anchorsForWire(s.outerWire, /*isHole=*/false);
    if (outerAnchorCount != nullptr)
        *outerAnchorCount = result.size();
    for (const TopoDS_Wire &holeWire : s.holeWires)
    {
        auto ha = anchorsForWire(holeWire, /*isHole=*/true);
        result.insert(result.end(), ha.begin(), ha.end());
    }
    if (outAnchorWallPts != nullptr)
        *outAnchorWallPts = std::move(allRuns);
    return result;
}

std::vector<StrutAnchor> BuildStrutAnchorPreviewPoints(const Face *face,
                                                       const BakeParams &params)
{
    if (face == nullptr)
        return {};

    StrutSetup s;
    if (!BuildStrutSetup(face, params, s))
        return {};

    return ComputeStrutWallAnchors(s, params);
}

struct StrutSegmentFull
{
    glm::vec3 a, b;   // endpoints in flat-projection space
    glm::vec2 nA, nB; // inward wall normals at a and b respectively
    std::vector<glm::dvec2> wallPtsA, wallPtsB; // each end's own wall-run polyline, local-flat 2D
};

// Forward-declared: defined further below alongside BuildFusedStrutQuadsShapeAtZ, the production
// geometry builder it primarily serves — also used during candidate selection (see
// StrutCandidateFitsWallClip below) so a candidate is only accepted if its real, wall-clipped cap
// geometry actually builds, instead of approximating that with a cheap 2D quad test.
// `outTaperPenalty`, if non-null, receives how much narrower than `halfWidth` either end had to
// shrink to actually land on its wall, averaged over all four corners and normalized to [0,1) — 0
// when both ends kept full width. Production geometry call sites don't need this and omit it.
static TopoDS_Face BuildStrutQuadByWallClip(const StrutSegmentFull &seg, double halfWidth,
                                            double chordTolMm, double zPlane,
                                            double *outTaperPenalty = nullptr);

// Candidate-selection width check: builds the strut's real wall-clipped cap geometry (the same
// boolean BuildFusedStrutQuadsShapeAtZ uses for final geometry) and accepts the candidate only if
// it actually produces a non-degenerate face. Replaces the older StrutQuadIsValid 2D-quad
// approximation, which could reject candidates the real clip handles fine (e.g. non-mitered
// corners near oblique walls) or accept ones it can't. Results are cached per (face, anchor i,
// anchor j, insetMm, chordTolMm) in StrutQuadValidityCache since this is an OCCT boolean, not a
// cheap test, and the production geometry / preview overlay / candidate-list overlay each
// independently re-walk every anchor pair for the same face within one user action.
struct StrutFitResult { bool valid; double taperPenalty; };
static StrutFitResult StrutCandidateFitsWallClip(const Face *face, int i, int j, const glm::dvec2 &a,
                                                 const glm::dvec2 &b, const glm::dvec2 &nA, const glm::dvec2 &nB,
                                                 const std::vector<glm::dvec2> &wallPtsA,
                                                 const std::vector<glm::dvec2> &wallPtsB, double halfWidth,
                                                 double insetMm, double chordTolMm, float zPlane)
{
    const StrutQuadCacheKey key{face, i, j, insetMm, chordTolMm};
    auto &cache = StrutQuadValidityCache();
    if (auto it = cache.find(key); it != cache.end())
        return {it->second.first, it->second.second};

    const StrutSegmentFull testSeg{glm::vec3(a.x, a.y, zPlane), glm::vec3(b.x, b.y, zPlane),
                                   glm::vec2(nA), glm::vec2(nB), wallPtsA, wallPtsB};
    double taperPenalty = 0.0;
    const TopoDS_Face quad = BuildStrutQuadByWallClip(testSeg, halfWidth, chordTolMm, zPlane, &taperPenalty);
    bool valid = false;
    if (!quad.IsNull())
    {
        GProp_GProps props;
        BRepGProp::SurfaceProperties(quad, props);
        valid = std::abs(props.Mass()) > 1e-6;
    }
    cache.emplace(key, std::make_pair(valid, taperPenalty));
    return {valid, taperPenalty};
}

static std::vector<StrutSegmentFull> BuildStrutSegmentsFull(const Face *face, const BakeParams &params)
{
    if (face == nullptr)
        return {};

    StrutSetup s;
    if (!BuildStrutSetup(face, params, s))
        return {};

    // Same anchors as the preview-overlay points, so struts always terminate
    // exactly where the anchor markers are drawn.
    struct WallAnchor { glm::dvec2 mid2D; glm::dvec2 N; std::vector<glm::dvec2> wallPts; };
    std::vector<WallAnchor> anchors;
    size_t outerAnchorCount = 0;
    std::vector<std::vector<glm::dvec2>> anchorWallPts;
    const std::vector<StrutAnchor> rawAnchors =
        ComputeStrutWallAnchors(s, params, &outerAnchorCount, &anchorWallPts);
    for (size_t ai = 0; ai < rawAnchors.size(); ++ai)
    {
        const StrutAnchor &a = rawAnchors[ai];
        anchors.push_back({glm::dvec2(a.pos.x, a.pos.y), glm::dvec2(a.N.x, a.N.y),
                           ai < anchorWallPts.size() ? anchorWallPts[ai] : std::vector<glm::dvec2>{}});
    }

    if (anchors.size() < 2)
    {
        std::cerr << "[strutpair] face=" << face << " anchors=" << anchors.size() << " -> too few anchors\n";
        return {};
    }

    // Collect carve-region rings directly from the wires anchors are placed on (s.outerWire,
    // s.holeWires) and convert with the same toFlatInv used for mid2D — guarantees the rings
    // line up exactly with the anchors instead of risking a frame mismatch from a separately
    // built footprint shape. allRings2D[0] is always the outer ring (outerWire pushed first).
    auto wireToRing2D = [&s](const TopoDS_Wire &wire, double chordTolMm) -> std::vector<glm::dvec2>
    {
        std::vector<glm::dvec2> ring2D;
        for (const glm::dvec3 &p : ExtractRingPointsInWireOrder(wire, chordTolMm))
        {
            const gp_Pnt w = gp_Pnt(p.x, p.y, p.z).Transformed(s.toFlatInv);
            ring2D.push_back({w.X(), w.Y()});
        }
        return ring2D;
    };

    std::vector<std::vector<glm::dvec2>> allRings2D;
    allRings2D.push_back(wireToRing2D(s.outerWire, params.chordTolMm));
    for (const TopoDS_Wire &hole : s.holeWires)
        allRings2D.push_back(wireToRing2D(hole, params.chordTolMm));

    const double halfWidth = params.insetMm * 0.5;

    if (allRings2D.front().size() < 3)
    {
        std::cerr << "[strutpair] face=" << face << " outerRingPts=" << allRings2D.front().size()
                  << " -> degenerate outer ring\n";
        return {};
    }

    if (std::getenv("CAD_STRUT_RAW_DUMP") != nullptr)
    {
        std::cerr << "[strutdump] face=" << face << " anchors=" << anchors.size()
                  << " outerAnchorCount=" << outerAnchorCount << "\n";
        for (size_t ai = 0; ai < anchors.size(); ++ai)
        {
            std::cerr << "  anchor[" << ai << "] mid2D=(" << anchors[ai].mid2D.x << ","
                      << anchors[ai].mid2D.y << ") N=(" << anchors[ai].N.x << ","
                      << anchors[ai].N.y << ") runPts=" << anchors[ai].wallPts.size();
            if (!anchors[ai].wallPts.empty())
            {
                const glm::dvec2 &f = anchors[ai].wallPts.front();
                const glm::dvec2 &b = anchors[ai].wallPts.back();
                std::cerr << " runStart=(" << f.x << "," << f.y << ") runEnd=(" << b.x << ","
                          << b.y << ") runLen=" << glm::length(b - f);
            }
            std::cerr << "\n";
        }
        for (size_t ri = 0; ri < allRings2D.size(); ++ri)
        {
            const auto &ring = allRings2D[ri];
            double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
            for (const glm::dvec2 &p : ring)
            {
                minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
                minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
            }
            std::cerr << "  ring[" << ri << "] pts=" << ring.size() << " bbox=(" << minX << ","
                      << minY << ")-(" << maxX << "," << maxY << ")\n";
        }
        std::cerr << "  halfWidth=" << halfWidth << "\n";
    }

    const int m = static_cast<int>(anchors.size());

    // Score a single strut's angle against one wall: 45° from the wall's inward normal is
    // ideal (a diagonal brace), 0°/90° (perpendicular/parallel to the wall) is worst.
    auto angleScoreFor = [](const glm::dvec2 &dir, const glm::dvec2 &N) -> double
    {
        const double cosA = glm::clamp(std::abs(glm::dot(dir, N)), 0.0, 1.0);
        const double angleDeg = glm::degrees(std::acos(cosA));
        return 1.0 - std::abs(angleDeg - 45.0) / 45.0;
    };

    // Pre-pass: find the longest valid outer-to-outer strut — used as the reference scale for
    // filtering hole-to-outer struts that are too short to provide useful cross-bracing.
    double maxOuterLen = 0.0;
    for (int i = 0; i < static_cast<int>(outerAnchorCount); ++i)
    {
        for (int j = i + 1; j < static_cast<int>(outerAnchorCount); ++j)
        {
            const double length = glm::length(anchors[j].mid2D - anchors[i].mid2D);
            if (length < 1e-9)
                continue;
            if (!StrutIsValid(anchors[i].mid2D, anchors[j].mid2D, allRings2D))
                continue;
            maxOuterLen = std::max(maxOuterLen, length);
        }
    }
    // A hole-to-outer strut must span at least this fraction of the longest outer-outer strut.
    // Below this threshold the strut is just a short stub from an interior hole boundary to a
    // nearby outer wall — it looks visually incomplete (strut not reaching the far edge) and
    // provides negligible cross-bracing.
    constexpr double kHoleStrutMinFrac = 0.35;

    // Every valid strut, indexed by each of its two anchors with direction pointing away from
    // that anchor — lets us enumerate, for a given anchor/edge, all pairs of struts leaving it.
    struct StrutCand { int other; glm::dvec2 dir; double length; double angleScore; double taperPenalty; };
    std::vector<std::vector<StrutCand>> byAnchor(m);
    double maxLen = 0.0;
    int dbgTooShort = 0, dbgStub = 0, dbgFailCenterline = 0, dbgFailQuad = 0, dbgTotal = 0;
    int dbgCrossOuter = 0, dbgCrossHole = 0, dbgMidOutside = 0, dbgMidInHole = 0;
    const bool dbgVerbose = std::getenv("CAD_STRUT_RAW_DUMP") != nullptr;

    for (int i = 0; i < m; ++i)
    {
        for (int j = i + 1; j < m; ++j)
        {
            ++dbgTotal;
            // Hole-to-hole pairs are no longer banned outright: StrutIsValid (below) already
            // rejects a strut whose midpoint lands inside any hole's void, which is the only
            // genuinely invalid case (e.g. two anchors on the same convex/circular hole — any
            // chord between them necessarily passes through that hole's interior). A pair that
            // survives StrutIsValid is real solid-material bracing regardless of which wire(s)
            // its anchors sit on, so it's judged by the same length/scoring rules as any other.
            const glm::dvec2 delta = anchors[j].mid2D - anchors[i].mid2D;
            const double length = glm::length(delta);
            if (length < 1e-9)
            {
                ++dbgTooShort;
                continue;
            }
            // Hole-involving struts shorter than kHoleStrutMinFrac × maxOuterLen are stubs that
            // terminate at a nearby wall rather than spanning toward the far side of the face.
            const bool isHolePair = (static_cast<size_t>(i) >= outerAnchorCount) ||
                                    (static_cast<size_t>(j) >= outerAnchorCount);
            if (isHolePair && maxOuterLen > 1e-9 && length < maxOuterLen * kHoleStrutMinFrac)
            {
                ++dbgStub;
                continue;
            }
            int dbgReason = -1;
            if (!StrutIsValid(anchors[i].mid2D, anchors[j].mid2D, allRings2D, &dbgReason))
            {
                ++dbgFailCenterline;
                switch (dbgReason)
                {
                    case 0: ++dbgCrossOuter; break;
                    case 1: ++dbgCrossHole; break;
                    case 2: ++dbgMidOutside; break;
                    case 3: ++dbgMidInHole; break;
                }
                if (dbgVerbose)
                    std::cerr << "  pair(" << i << "," << j << ") len=" << length
                              << " centerlineFail reason=" << dbgReason << "\n";
                continue;
            }
            const StrutFitResult fit = StrutCandidateFitsWallClip(
                face, i, j, anchors[i].mid2D, anchors[j].mid2D, anchors[i].N, anchors[j].N,
                anchors[i].wallPts, anchors[j].wallPts, halfWidth, params.insetMm,
                params.chordTolMm, s.zPlane);
            if (!fit.valid)
            {
                ++dbgFailQuad;
                if (dbgVerbose)
                    std::cerr << "  pair(" << i << "," << j << ") len=" << length << " quadFail\n";
                continue;
            }
            const glm::dvec2 dirIJ = delta / length;
            // Angle score covers both walls the strut connects to.
            const double angleScore = 0.5 * (angleScoreFor(dirIJ, anchors[i].N) +
                                             angleScoreFor(-dirIJ, anchors[j].N));
            byAnchor[i].push_back({j, dirIJ, length, angleScore, fit.taperPenalty});
            byAnchor[j].push_back({i, -dirIJ, length, angleScore, fit.taperPenalty});
            maxLen = std::max(maxLen, length);
        }
    }
    if (maxLen < 1e-9)
    {
        std::cerr << "[strutpair] face=" << face << " anchors=" << m
                  << " -> no valid struts"
                  << " [dbg total=" << dbgTotal << " tooShort=" << dbgTooShort
                  << " stub=" << dbgStub << " failCenterline=" << dbgFailCenterline
                  << " (crossOuter=" << dbgCrossOuter << " crossHole=" << dbgCrossHole
                  << " midOutside=" << dbgMidOutside << " midInHole=" << dbgMidInHole << ")"
                  << " failQuad=" << dbgFailQuad << "]\n";
        return {};
    }

    // Joint pair scoring: for each edge anchor, evaluate every pair of struts leaving it as a
    // unit (rather than greedily picking the single best strut, then searching for its best
    // partner — that can strand a great strut with only mediocre partners). A pair scores well
    // when both struts sit close to 45° from the walls they touch, are long, and their bisector
    // — the bracing pattern's natural symmetry axis — aligns with this edge's inward normal.
    constexpr double kAngleWeight     = 1.0;
    constexpr double kLengthWeight    = 1.0;
    constexpr double kSymmetryWeight  = 1.0;
    // Penalizes struts whose endpoint(s) had to taper narrower than insetMm/2 to actually land on
    // their wall (see BuildStrutQuadByWallClip's outTaperPenalty) — a tapered candidate is still
    // buildable and gets emitted if it's the only option at this anchor, but loses to a full-width
    // alternative when one exists, since a tapered tip carries less material than its length alone
    // would suggest.
    constexpr double kTaperWeight     = 1.0;

    // One brace pair per wall: for every anchor, keep its single highest-scoring pair of
    // struts (rather than one pair for the whole face) — every outline edge gets its own
    // symmetric V into the interior.
    std::vector<StrutSegmentFull> segments;
    int pairsEmitted = 0;
    int singlesEmitted = 0;

    for (int i = 0; i < m; ++i)
    {
        const auto &cands = byAnchor[i];
        if (cands.empty())
            continue;

        if (cands.size() == 1)
        {
            const glm::vec3 origin(anchors[i].mid2D.x, anchors[i].mid2D.y, s.zPlane);
            const StrutCand &only = cands.front();
            segments.push_back({origin,
                                 glm::vec3(anchors[only.other].mid2D.x, anchors[only.other].mid2D.y, s.zPlane),
                                 glm::vec2(anchors[i].N),
                                 glm::vec2(anchors[only.other].N),
                                 anchors[i].wallPts, anchors[only.other].wallPts});
            ++singlesEmitted;
            continue;
        }
        double bestScore = -1.0;
        int bestJ = -1, bestK = -1;
        for (size_t a = 0; a < cands.size(); ++a)
        {
            for (size_t b = a + 1; b < cands.size(); ++b)
            {
                const StrutCand &c1 = cands[a];
                const StrutCand &c2 = cands[b];
                const glm::dvec2 bisector = c1.dir + c2.dir;
                const double bisectorLen = glm::length(bisector);
                const double symmetryScore = bisectorLen > 1e-9
                    ? glm::clamp(glm::dot(bisector / bisectorLen, anchors[i].N), 0.0, 1.0)
                    : 0.0;
                const double angleTerm  = 0.5 * (c1.angleScore + c2.angleScore);
                const double lengthTerm = 0.5 * (c1.length + c2.length) / maxLen;
                const double taperTerm  = 0.5 * (c1.taperPenalty + c2.taperPenalty);
                const double score = kAngleWeight * angleTerm + kLengthWeight * lengthTerm +
                                     kSymmetryWeight * symmetryScore - kTaperWeight * taperTerm;
                if (score > bestScore)
                {
                    bestScore = score;
                    bestJ = c1.other;
                    bestK = c2.other;
                }
            }
        }
        if (bestJ < 0)
            continue;

        const glm::vec3 origin(anchors[i].mid2D.x, anchors[i].mid2D.y, s.zPlane);
        segments.push_back({origin,
                             glm::vec3(anchors[bestJ].mid2D.x, anchors[bestJ].mid2D.y, s.zPlane),
                             glm::vec2(anchors[i].N), glm::vec2(anchors[bestJ].N),
                             anchors[i].wallPts, anchors[bestJ].wallPts});
        segments.push_back({origin,
                             glm::vec3(anchors[bestK].mid2D.x, anchors[bestK].mid2D.y, s.zPlane),
                             glm::vec2(anchors[i].N), glm::vec2(anchors[bestK].N),
                             anchors[i].wallPts, anchors[bestK].wallPts});
        ++pairsEmitted;
    }

    int totalCands = 0;
    for (const auto &v : byAnchor) totalCands += static_cast<int>(v.size());

    std::cerr << "[strutpair] face=" << face << " anchors=" << m
              << " validStruts=" << (totalCands / 2) << " pairsEmitted=" << pairsEmitted
              << " singlesEmitted=" << singlesEmitted << " segments=" << segments.size() << "\n";

    return segments;
}

std::vector<std::pair<glm::vec3, glm::vec3>> BuildStrutPreviewLines(const Face *face,
                                                                    const BakeParams &params)
{
    std::vector<std::pair<glm::vec3, glm::vec3>> out;
    for (const auto &s : BuildStrutSegmentsFull(face, params))
        out.push_back({s.a, s.b});
    return out;
}

/// Returns each strut's final footprint-clipped quad outline as line segments — the exact same
/// wall-clip + BRepAlgoAPI_Common(footprint) geometry BuildFusedStrutQuadsShapeAtZ fuses into the
/// production solid — plus its centerline. Kept in lockstep with the production path so this
/// debug overlay can never show a strut crossing a hole or the outer wall that the actual carved
/// result wouldn't: anything the footprint clip below trims away is trimmed here too.
std::vector<std::pair<glm::vec3, glm::vec3>> BuildStrutQuadPreviewLines(const Face *face,
                                                                         const BakeParams &params)
{
    std::vector<std::pair<glm::vec3, glm::vec3>> out;
    if (face == nullptr)
        return out;

    const auto segs = BuildStrutSegmentsFull(face, params);
    if (segs.empty())
        return out;

    const float zPlane = segs.front().a.z;
    const double halfWidth = params.insetMm * 0.5;

    const std::vector<std::vector<glm::dvec3>> baseRings =
        StructurePreviewUsesXyCarveFootprint() ? BuildXyCarveFootprintPreviewRings(face, params)
                                               : BuildTrimmedFacePlanePreviewRings(face, params);
    TopoDS_Face footprintFace;
    if (!baseRings.empty() && baseRings.front().size() >= 3)
    {
        const std::vector<glm::dvec3> &outer = baseRings.front();
        const std::vector<std::vector<glm::dvec3>> holes(baseRings.begin() + 1, baseRings.end());
        footprintFace = MakeFlatFaceFromOuterAndHoles(outer, holes, zPlane);
    }

    for (const auto &seg : segs)
    {
        const TopoDS_Face wallClipped =
            BuildStrutQuadByWallClip(seg, halfWidth, params.chordTolMm, zPlane);
        if (wallClipped.IsNull())
            continue;

        std::vector<TopoDS_Face> clippedFaces;
        if (!footprintFace.IsNull())
        {
            BRepAlgoAPI_Common common(wallClipped, footprintFace);
            common.SetFuzzyValue(1e-3);
            common.Build();
            if (common.IsDone() && !common.Shape().IsNull())
                for (TopExp_Explorer exp(common.Shape(), TopAbs_FACE); exp.More(); exp.Next())
                    clippedFaces.push_back(TopoDS::Face(exp.Current()));
        }
        if (clippedFaces.empty())
            clippedFaces.push_back(wallClipped);

        for (const TopoDS_Face &quad : clippedFaces)
        {
            const TopoDS_Wire outer = BRepTools::OuterWire(quad);
            if (outer.IsNull())
                continue;
            const std::vector<glm::dvec3> ring = ExtractRingPointsInWireOrder(outer, params.chordTolMm);
            for (size_t i = 0; i + 1 < ring.size(); ++i)
                out.push_back({glm::vec3(ring[i]), glm::vec3(ring[i + 1])});
            if (ring.size() > 2)
                out.push_back({glm::vec3(ring.back()), glm::vec3(ring.front())});
        }
        // also draw the centerline
        out.push_back({seg.a, seg.b});
    }
    return out;
}

std::vector<std::pair<glm::vec3, glm::vec3>> BuildNotchedCutOutlinePreviewLines(const Face *face,
                                                                                const BakeParams &params)
{
    std::vector<std::pair<glm::vec3, glm::vec3>> segments;
    if (face == nullptr)
        return segments;

    TopoDS_Shape footprint = BuildCurvedCarveFootprintMvp(face, params);
    if (footprint.IsNull())
        return segments;

    // Notch strut rectangles out (rectangular corners — no ChFi2d rounding).
    footprint = NotchStrutsIntoCurvedFootprint(face, params, footprint);

    glm::dvec3 faceNormal{0.0, 0.0, 1.0};
    if (face->surface != nullptr)
    {
        faceNormal = face->surface->GetNormal();
        const double nLen = glm::length(faceNormal);
        if (nLen > 1e-12)
            faceNormal /= nLen;
    }
    const glm::dvec3 lift = faceNormal * 0.06;

    for (TopExp_Explorer exWire(footprint, TopAbs_WIRE); exWire.More(); exWire.Next())
    {
        const std::vector<glm::dvec3> ring =
            ExtractRingPointsInWireOrder(TopoDS::Wire(exWire.Current()), params.chordTolMm);
        if (ring.size() < 3)
            continue;
        for (size_t i = 0; i < ring.size(); ++i)
        {
            const glm::dvec3 a = ring[i] + lift;
            const glm::dvec3 b = ring[(i + 1) % ring.size()] + lift;
            segments.push_back({glm::vec3(a.x, a.y, a.z), glm::vec3(b.x, b.y, b.z)});
        }
    }
    return segments;
}

std::vector<std::pair<glm::vec3, glm::vec3>> BuildAllStrutCandidatePreviewLines(const Face *face,
                                                                                const BakeParams &params)
{
    if (face == nullptr)
        return {};

    StrutSetup s;
    if (!BuildStrutSetup(face, params, s))
        return {};

    struct WallAnchor { glm::dvec2 mid2D; glm::dvec2 N; std::vector<glm::dvec2> wallPts; };
    std::vector<WallAnchor> anchors;
    size_t outerAnchorCount = 0;
    std::vector<std::vector<glm::dvec2>> anchorWallPts;
    const std::vector<StrutAnchor> rawAnchors =
        ComputeStrutWallAnchors(s, params, &outerAnchorCount, &anchorWallPts);
    for (size_t ai = 0; ai < rawAnchors.size(); ++ai)
    {
        const StrutAnchor &a = rawAnchors[ai];
        anchors.push_back({glm::dvec2(a.pos.x, a.pos.y), glm::dvec2(a.N.x, a.N.y),
                           ai < anchorWallPts.size() ? anchorWallPts[ai] : std::vector<glm::dvec2>{}});
    }

    if (anchors.size() < 2)
        return {};

    auto wireToRing2D = [&s](const TopoDS_Wire &wire, double chordTolMm) -> std::vector<glm::dvec2>
    {
        std::vector<glm::dvec2> ring2D;
        for (const glm::dvec3 &p : ExtractRingPointsInWireOrder(wire, chordTolMm))
        {
            const gp_Pnt w = gp_Pnt(p.x, p.y, p.z).Transformed(s.toFlatInv);
            ring2D.push_back({w.X(), w.Y()});
        }
        return ring2D;
    };

    std::vector<std::vector<glm::dvec2>> allRings2D;
    allRings2D.push_back(wireToRing2D(s.outerWire, params.chordTolMm));
    for (const TopoDS_Wire &hole : s.holeWires)
        allRings2D.push_back(wireToRing2D(hole, params.chordTolMm));

    if (allRings2D.front().size() < 3)
        return {};

    const int m = static_cast<int>(anchors.size());
    const double halfWidth = params.insetMm * 0.5;

    // Pre-pass: maxOuterLen for hole-strut length filter (mirrors BuildStrutPreviewLines).
    double maxOuterLen = 0.0;
    for (int i = 0; i < static_cast<int>(outerAnchorCount); ++i)
        for (int j = i + 1; j < static_cast<int>(outerAnchorCount); ++j)
        {
            const double length = glm::length(anchors[j].mid2D - anchors[i].mid2D);
            if (length < 1e-9) continue;
            if (!StrutIsValid(anchors[i].mid2D, anchors[j].mid2D, allRings2D)) continue;
            maxOuterLen = std::max(maxOuterLen, length);
        }
    constexpr double kHoleStrutMinFrac = 0.35;

    // Emit every valid (i,j) pair without best-pair selection so the user can see which
    // candidates exist before the scoring algorithm narrows them down.
    const bool dbgVerbose = std::getenv("CAD_STRUT_RAW_DUMP") != nullptr;
    std::vector<std::pair<glm::vec3, glm::vec3>> segments;
    for (int i = 0; i < m; ++i)
    {
        for (int j = i + 1; j < m; ++j)
        {
            // Hole-to-hole pairs are no longer banned outright — see BuildStrutSegmentsFull.
            const glm::dvec2 delta = anchors[j].mid2D - anchors[i].mid2D;
            const double length = glm::length(delta);
            if (length < 1e-9)
                continue;
            const bool isHolePair = (static_cast<size_t>(i) >= outerAnchorCount) ||
                                    (static_cast<size_t>(j) >= outerAnchorCount);
            if (isHolePair && maxOuterLen > 1e-9 && length < maxOuterLen * kHoleStrutMinFrac)
            {
                if (dbgVerbose)
                    std::cerr << "  allcand pair(" << i << "," << j << ") len=" << length
                              << " rejected: holeStub (maxOuterLen=" << maxOuterLen << ")\n";
                continue;
            }
            int dbgReason = -1;
            if (!StrutIsValid(anchors[i].mid2D, anchors[j].mid2D, allRings2D, &dbgReason))
            {
                if (dbgVerbose)
                    std::cerr << "  allcand pair(" << i << "," << j << ") len=" << length
                              << " rejected: centerline reason=" << dbgReason << "\n";
                continue;
            }
            if (!StrutCandidateFitsWallClip(face, i, j, anchors[i].mid2D, anchors[j].mid2D,
                                           anchors[i].N, anchors[j].N, anchors[i].wallPts,
                                           anchors[j].wallPts, halfWidth, params.insetMm,
                                           params.chordTolMm, s.zPlane).valid)
            {
                if (dbgVerbose)
                    std::cerr << "  allcand pair(" << i << "," << j << ") len=" << length
                              << " rejected: wallClip (halfWidth=" << halfWidth << ")\n";
                continue;
            }
            if (dbgVerbose)
                std::cerr << "  allcand pair(" << i << "," << j << ") len=" << length << " accepted\n";
            const glm::vec3 a(anchors[i].mid2D.x, anchors[i].mid2D.y, s.zPlane);
            const glm::vec3 b(anchors[j].mid2D.x, anchors[j].mid2D.y, s.zPlane);
            segments.push_back({a, b});
        }
    }
    if (dbgVerbose)
    {
        std::cerr << "[allcanddump] face=" << face << " anchors=" << m
                  << " outerAnchorCount=" << outerAnchorCount << " halfWidth=" << halfWidth << "\n";
        for (size_t ai = 0; ai < anchors.size(); ++ai)
            std::cerr << "  anchor[" << ai << "] mid2D=(" << anchors[ai].mid2D.x << ","
                      << anchors[ai].mid2D.y << ") runPts=" << anchors[ai].wallPts.size() << "\n";
    }
    return segments;
}

// Forward-declared: defined further below, alongside the cut-outline ring pipeline it primarily
// serves — reused here to turn the merged strut-quad outline into preview line segments.
std::vector<std::pair<glm::vec3, glm::vec3>> RingsToPreviewLineSegments(
    const std::vector<std::vector<glm::dvec3>> &rings, const glm::dvec3 &previewLift);

/// Builds the fused union of every strut's squared-off quad — each strut is built deliberately
/// *oversized* (its centerline extended past both wall anchors by `insetMm * 6`) and then clipped
/// to the carve-region footprint via `BRepAlgoAPI_Common` (intersection). OCCT does the squaring
/// off for us: the boolean finds exactly where the oversized rectangle crosses the *real* footprint
/// boundary (curves, fillets and all) and trims it there — no manual polyline-crossing search, no
/// "couldn't find a landing within the search bound" failure mode, and no discretization gap
/// between the trimmed end and the wall it connects to (they become the literal same edge). All
/// surviving clipped quads are fused into one shape stamped at `zPlane`. Shared by the rail preview
/// (converted to boundary-ring line segments, at the struts' own preview-lift Z) and the
/// cut-outline notch (subtracted from the carve footprint, at the footprint's Z) so both stay in
/// lockstep — the carve preview's notch boundary exactly traces the strut quads it shows as
/// preserved material, with no possibility of the two drifting apart since they share one geometry
/// source. Only the XY shape is shared — Z only places the result, so each caller can stamp it onto
/// its own plane.
// When `skipFootprintClip` is true the polygon-footprint BRepAlgoAPI_Common step is omitted and
// the raw mitered quads (which already extend to/past the source ring, i.e. outside the inset
// footprint) are returned unclipped. This lets the caller (NotchStrutsIntoCurvedFootprint) use
// BRepAlgoAPI_Cut against the *curved* OCCT footprint so the struts are trimmed at the exact arc
// edge — avoiding the chord-vs-arc crescent gap that arises when the polygon clip runs first.
// Finds where the infinite line through `p0` along `dir` crosses `wallPts` (a polyline, open or
// closed), returning the crossing parameter `t` (signed distance along `dir` from `p0`) closest
// to `targetT`. `targetT` biases the pick toward the end the caller actually cares about when a
// wall run has more than one crossing (0 for the near/A-side trim, `len` for the far/B-side
// trim). Returns false with `outT` untouched if the line never crosses the run.
static bool RayPolylineIntersect(const glm::dvec2 &p0, const glm::dvec2 &dir,
                                 const std::vector<glm::dvec2> &wallPts, double targetT,
                                 double &outT)
{
    bool found = false;
    double bestT = 0.0, bestDist = std::numeric_limits<double>::max();
    for (size_t k = 0; k + 1 < wallPts.size(); ++k)
    {
        const glm::dvec2 &p1 = wallPts[k];
        const glm::dvec2 segDir = wallPts[k + 1] - p1;
        const double denom = dir.x * segDir.y - dir.y * segDir.x;
        if (std::abs(denom) < 1e-12)
            continue; // parallel to this segment
        const glm::dvec2 diff = p1 - p0;
        const double s = (diff.x * dir.y - diff.y * dir.x) / denom;
        if (s < -1e-9 || s > 1.0 + 1e-9)
            continue; // crossing falls outside this segment
        const double t = (diff.x * segDir.y - diff.y * segDir.x) / denom;
        const double dist = std::abs(t - targetT);
        if (dist < bestDist)
        {
            bestDist = dist;
            bestT = t;
            found = true;
        }
    }
    if (found)
        outT = bestT;
    return found;
}

// True if `wallPts` is straight to within `tolMm` of the line through its two ends. A straight
// run is sampled with zero curve-deviation error (it has no "true curve" to fall short of), so it
// needs no overshoot at all; only a genuinely curved run does.
static bool RunIsStraight(const std::vector<glm::dvec2> &wallPts, double tolMm)
{
    if (wallPts.size() < 3)
        return true;
    const glm::dvec2 &p0 = wallPts.front();
    const glm::dvec2 chord = wallPts.back() - p0;
    const double chordLen = glm::length(chord);
    if (chordLen < 1e-9)
        return true;
    const glm::dvec2 normal(-chord.y / chordLen, chord.x / chordLen);
    for (const glm::dvec2 &p : wallPts)
        if (std::abs(glm::dot(p - p0, normal)) > tolMm)
            return false;
    return true;
}

// Extrapolates an open run's two ends, each along that end's own local tangent, by `margin`.
// Fallback only: used when the strut edge's true crossing lies past where this anchor's own run
// was sampled — e.g. a strut approaching right where a straight wall hands off to an adjoining
// fillet arc that is its own separate run, so the straight run's points alone never cross the
// offset edge. The extrapolation is a straight line, not the real adjoining curve, so it can be
// geometrically off — but that's fixed up by the downstream Common/Cut against the real curved
// footprint (same safety net the chordTol overshoot below relies on); the alternative, leaving
// the edge stuck at the run's last sampled point, is the actual undershoot bug being fixed here.
static std::vector<glm::dvec2> ExtendRunEnds(const std::vector<glm::dvec2> &wallPts, double margin)
{
    if (wallPts.size() < 2 || glm::distance(wallPts.front(), wallPts.back()) < 1e-6)
        return wallPts; // too short to have a tangent, or already a closed loop
    std::vector<glm::dvec2> extended;
    extended.reserve(wallPts.size() + 2);
    const glm::dvec2 startDelta = wallPts.front() - wallPts[1];
    const double startLen = glm::length(startDelta);
    if (startLen > 1e-12)
        extended.push_back(wallPts.front() + (startDelta / startLen) * margin);
    extended.insert(extended.end(), wallPts.begin(), wallPts.end());
    const size_t n = wallPts.size();
    const glm::dvec2 endDelta = wallPts[n - 1] - wallPts[n - 2];
    const double endLen = glm::length(endDelta);
    if (endLen > 1e-12)
        extended.push_back(wallPts.back() + (endDelta / endLen) * margin);
    return extended;
}

// Builds one strut's quad by trimming each of its two long edges directly against the wall
// polyline at each end — replaces the previous "build an oversized rectangle, intersect against
// an artificial wall-clip region, pick a result face" approach (an OCCT boolean plus a
// largest-area fallback that could pick the wrong, oversized fragment on a curved/split result,
// which is what let some struts come out longer than intended). A strut's long edge is a straight
// line for its whole length, so the same line that passes anchor A's offset point also passes
// anchor B's — trimming each end is just "where does this line cross that end's own wall run",
// biased toward the end in question via `targetT` so a wall run with multiple crossings still
// picks the one that actually bounds this strut. No boolean ops, no ambiguous multi-fragment
// pick: if a wall run doesn't reach an edge's offset line, that edge falls back to a bounded
// tangent extrapolation (see ExtendRunEnds/maxExtendReach below); if even that doesn't land near
// the run, this end never actually touches the wall it's meant to connect to (e.g. the strut is
// wider than the local feature, such as a small fillet) and the whole candidate is rejected.
static TopoDS_Face BuildStrutQuadByWallClip(const StrutSegmentFull &seg, double halfWidth,
                                            double chordTolMm, double zPlane,
                                            double *outTaperPenalty)
{
    const glm::dvec2 a2(seg.a.x, seg.a.y);
    const glm::dvec2 b2(seg.b.x, seg.b.y);
    const glm::dvec2 delta = b2 - a2;
    const double len = glm::length(delta);
    if (len < 1e-9)
        return TopoDS_Face();
    const glm::dvec2 dir = delta / len;
    const glm::dvec2 perp(-dir.y, dir.x);

    // wallPts is only a chord-discretized approximation of the real wall curve (sampled to within
    // chordTolMm of it, see GCPnts_QuasiUniformDeflection), and a polygon chord always sits
    // slightly inside the true arc it approximates. Stopping exactly at the polyline crossing
    // therefore stops short of the real wall on a curved/filleted run — but a straight run has no
    // such gap (the polyline matches it exactly), so the nudge below only applies where there's an
    // actual curve to fall short of. The downstream Common/Cut against the real (analytic)
    // footprint geometry trims any of this back to the exact boundary, same pattern this file
    // already relies on elsewhere (see BuildFusedStrutQuadsShapeAtZ's skipFootprintClip comment) —
    // overshoot here is harmless, undershoot is not, since nothing past this point can extend a
    // strut, only shrink it.
    const double wallOvershoot = std::max(chordTolMm * 1.5, 1e-3);
    const double extendMargin = std::max({len * 2.0, halfWidth * 20.0, 5.0});
    // How far past this run's own sampled extent the tangent-extrapolation fallback is trusted to
    // reach. A small reach covers the legitimate case (a straight wall handing off to its own
    // adjoining fillet run, not sampled here). A strut whose half-width is larger than the local
    // wall feature it's terminating against (e.g. a small fillet narrower than the strut) has no
    // real crossing nearby at all -- the fallback would still eventually find *some* crossing far
    // down the extrapolated tangent line, but that's not this end actually touching its wall, just
    // the math finding an arbitrary point. Capping the trusted reach turns that into a rejected
    // candidate instead of a strut edge that shoots off across the model.
    const double maxExtendReach = std::max(halfWidth * 3.0, 1e-3);
    // Same crossing search as before, but at an arbitrary lateral offset `width` instead of always
    // the full `halfWidth` — lets trimEndTapered (below) narrow a corner down until it finds a
    // wall it can actually reach.
    auto trimEndAtWidth = [&](double sign, const std::vector<glm::dvec2> &wallPts, double targetT,
                              double awayDir, double width) -> double
    {
        const glm::dvec2 p0 = a2 + perp * (sign * width);
        double t = targetT;
        bool found = RayPolylineIntersect(p0, dir, wallPts, targetT, t);
        // A wall run can be the entire straight edge an anchor sits on (sampled as just its two
        // endpoints), so a near-parallel strut can mathematically cross it at a `t` far from
        // `targetT` — a real point on the wall, but nowhere near where this corner is actually
        // supposed to land. Reject it here (same trust radius as the extension fallback below) so
        // it falls through to that fallback, or to outright failure, instead of being accepted as
        // a legitimate touch.
        if (found && std::abs(t - targetT) > maxExtendReach)
            found = false;
        if (!found)
        {
            double extT = targetT;
            if (RayPolylineIntersect(p0, dir, ExtendRunEnds(wallPts, extendMargin), targetT, extT) &&
                !wallPts.empty())
            {
                const glm::dvec2 candidate = p0 + dir * extT;
                const double reach = std::min(glm::distance(candidate, wallPts.front()),
                                              glm::distance(candidate, wallPts.back()));
                if (reach <= maxExtendReach)
                {
                    t = extT;
                    found = true;
                }
            }
        }
        if (!found)
            return std::numeric_limits<double>::quiet_NaN(); // this end never touches its wall
        if (RunIsStraight(wallPts, chordTolMm * 0.5))
            return t;
        return t + awayDir * wallOvershoot;
    };

    // A corner narrower than this fraction of halfWidth is rejected outright rather than tapered
    // and scored — below this point the strut tip is a near knife-edge: real stress-concentration
    // risk that a small area loss in the taper-penalty score wouldn't actually discourage, since
    // the lost area near a sharp tip is small even though the tip itself is structurally bad.
    constexpr double kMinTaperWidthFrac = 0.15;
    // Falls back to a taper when the full-width corner misses: binary-searches the narrowest
    // lateral offset (down from halfWidth toward the anchor's own wall-touching centerline at
    // width 0) that still lands on the wall, so the strut narrows just enough to reach instead of
    // being discarded whole. `outWidth` receives the width actually used (== halfWidth when no
    // taper was needed), so the caller can score how much material this corner gave up.
    auto trimEndTapered = [&](double sign, const std::vector<glm::dvec2> &wallPts, double targetT,
                              double awayDir, double *outWidth) -> double
    {
        const double tFull = trimEndAtWidth(sign, wallPts, targetT, awayDir, halfWidth);
        if (!std::isnan(tFull))
        {
            *outWidth = halfWidth;
            return tFull;
        }
        // width=0 puts the probe exactly on the strut's own centerline anchor, which sits on this
        // wall by construction (anchors are wall-edge points) — this should always succeed, but if
        // it somehow doesn't, there's truly nothing to taper toward and the end is rejected as before.
        const double tZero = trimEndAtWidth(sign, wallPts, targetT, awayDir, 0.0);
        if (std::isnan(tZero))
        {
            *outWidth = 0.0;
            return std::numeric_limits<double>::quiet_NaN();
        }
        double lo = 0.0, hi = halfWidth, bestT = tZero, bestWidth = 0.0;
        for (int iter = 0; iter < 16; ++iter)
        {
            const double mid = 0.5 * (lo + hi);
            const double tMid = trimEndAtWidth(sign, wallPts, targetT, awayDir, mid);
            if (!std::isnan(tMid))
            {
                lo = mid;
                bestT = tMid;
                bestWidth = mid;
            }
            else
            {
                hi = mid;
            }
        }
        if (bestWidth < halfWidth * kMinTaperWidthFrac)
        {
            *outWidth = bestWidth;
            return std::numeric_limits<double>::quiet_NaN(); // even tapered, too thin to be a real strut end
        }
        *outWidth = bestWidth;
        return bestT;
    };

    double wALeft = halfWidth, wARight = halfWidth, wBLeft = halfWidth, wBRight = halfWidth;
    const double tALeft = trimEndTapered(1.0, seg.wallPtsA, 0.0, -1.0, &wALeft);
    const double tARight = trimEndTapered(-1.0, seg.wallPtsA, 0.0, -1.0, &wARight);
    const double tBLeft = trimEndTapered(1.0, seg.wallPtsB, len, 1.0, &wBLeft);
    const double tBRight = trimEndTapered(-1.0, seg.wallPtsB, len, 1.0, &wBRight);
    if (std::getenv("CAD_STRUT_RAW_DUMP") != nullptr)
        std::cerr << "    quad a=(" << a2.x << "," << a2.y << ") b=(" << b2.x << "," << b2.y
                  << ") halfWidth=" << halfWidth << " floor=" << (halfWidth * kMinTaperWidthFrac)
                  << " wALeft=" << wALeft << "(t=" << tALeft << ") wARight=" << wARight
                  << "(t=" << tARight << ") wBLeft=" << wBLeft << "(t=" << tBLeft
                  << ") wBRight=" << wBRight << "(t=" << tBRight << ")\n";
    if (std::isnan(tALeft) || std::isnan(tARight) || std::isnan(tBLeft) || std::isnan(tBRight))
        return TopoDS_Face(); // an end never actually touched its wall, even tapered
    if (tBLeft <= tALeft + 1e-9 || tBRight <= tARight + 1e-9)
        return TopoDS_Face(); // degenerate after trimming

    if (outTaperPenalty != nullptr)
    {
        const double avgWidth = 0.25 * (wALeft + wARight + wBLeft + wBRight);
        *outTaperPenalty = halfWidth > 1e-12 ? glm::clamp(1.0 - avgWidth / halfWidth, 0.0, 1.0) : 0.0;
    }

    const glm::dvec2 cornerALeft = a2 + perp * wALeft + dir * tALeft;
    const glm::dvec2 cornerARight = a2 - perp * wARight + dir * tARight;
    const glm::dvec2 cornerBLeft = a2 + perp * wBLeft + dir * tBLeft;
    const glm::dvec2 cornerBRight = a2 - perp * wBRight + dir * tBRight;

    BRepBuilderAPI_MakePolygon poly;
    poly.Add(gp_Pnt(cornerALeft.x, cornerALeft.y, zPlane));
    poly.Add(gp_Pnt(cornerBLeft.x, cornerBLeft.y, zPlane));
    poly.Add(gp_Pnt(cornerBRight.x, cornerBRight.y, zPlane));
    poly.Add(gp_Pnt(cornerARight.x, cornerARight.y, zPlane));
    poly.Close();
    if (!poly.IsDone())
        return TopoDS_Face();
    BRepBuilderAPI_MakeFace mk(poly.Wire());
    return mk.IsDone() ? mk.Face() : TopoDS_Face();
}

static TopoDS_Shape BuildFusedStrutQuadsShapeAtZ(const Face *face, const BakeParams &params,
                                                  double zPlane, bool skipFootprintClip = false)
{
    TopoDS_Shape fusedStruts;
    if (face == nullptr || !(params.insetMm > 0.0))
        return fusedStruts;

    const auto strutSegments = BuildStrutSegmentsFull(face, params);
    if (strutSegments.empty())
        return fusedStruts;

    // The carve-region footprint (outer inset minus hole outsets) stamped at `zPlane` — every
    // oversized strut quad is clipped against this real boundary by intersection.
    // Only built when skipFootprintClip is false (the preview/polygon path).
    TopoDS_Face footprintFace;
    if (!skipFootprintClip)
    {
        const std::vector<std::vector<glm::dvec3>> baseRings =
            StructurePreviewUsesXyCarveFootprint() ? BuildXyCarveFootprintPreviewRings(face, params)
                                                   : BuildTrimmedFacePlanePreviewRings(face, params);
        if (baseRings.empty() || baseRings.front().size() < 3)
            return fusedStruts;
        const std::vector<glm::dvec3> &outer = baseRings.front();
        const std::vector<std::vector<glm::dvec3>> holes(baseRings.begin() + 1, baseRings.end());
        footprintFace = MakeFlatFaceFromOuterAndHoles(outer, holes, zPlane);
        if (footprintFace.IsNull())
            return fusedStruts;
    }

    const double halfWidth = params.insetMm * 0.5;

    for (const auto &seg : strutSegments)
    {
        // Each end is clipped against its own wall's real sampled geometry (BuildStrutQuadByWallClip)
        // rather than a closed-form trig miter — see the function's own comment for why. Endpoints
        // still sit on the wall, same as before; corner shaping just comes from a boolean now.
        const TopoDS_Face wallClipped =
            BuildStrutQuadByWallClip(seg, halfWidth, params.chordTolMm, zPlane);
        if (wallClipped.IsNull())
            continue;

        if (skipFootprintClip)
        {
            // Wall-clipped quad — endpoints sit on the inset boundary. The caller clips via
            // BRepAlgoAPI_Cut against the real curved footprint, which trims cleanly at the
            // analytic arc edges.
            fusedStruts = FuseCoplanarFaces(fusedStruts, wallClipped);
        }
        else
        {
            BRepAlgoAPI_Common common(wallClipped, footprintFace);
            common.SetFuzzyValue(1e-3);
            common.Build();
            if (!common.IsDone() || common.Shape().IsNull())
                continue;

            // The clip may come back as one or several coplanar faces (e.g. the wall-clipped
            // quad grazing a corner of the footprint); fuse them all in as this strut's quad.
            for (TopExp_Explorer exp(common.Shape(), TopAbs_FACE); exp.More(); exp.Next())
                fusedStruts = FuseCoplanarFaces(fusedStruts, TopoDS::Face(exp.Current()));
        }
    }

    return fusedStruts;
}

namespace
{

/// Replaces a closed XY ring's sufficiently sharp corners with circular arcs of radius `radiusMm`,
/// tangent to the two adjacent edges — the standard "round a polygon corner" construction: trim
/// back `radius * tan(turn/2)` along each edge (where `turn` is the angle between the edge
/// directions), with the arc center on the interior-angle bisector at `trim / sin(turn/2)` from
/// the corner and arc radius `trim / tan(turn/2)` (equal to `radiusMm` unless capped — see below).
/// Vertices whose turn is below `kMinTurnDeg` are left untouched: rounding a near-straight join
/// would be a no-op, and an already-tangent curve seam (e.g. a circular hole's start/end vertex)
/// has a turn of effectively zero — forcing a fillet there would be degenerate. The trim is capped
/// to half the shorter adjacent edge so the arc never eats into a neighbouring corner on a short
/// segment (shrinking the achieved radius there instead of corrupting the ring). Matches the
/// `insetMm`-drives-fillet-radius-1:1 convention used elsewhere in Structure (`BakeParams::insetMm`).
/// Operates purely on the discretized ring so the identical pass can be shared verbatim across
/// rings that must stay bit-identical (the cut outline and the strut-notch subset extracted from it).
/// True if open segments `[a0,a1]` and `[b0,b1]` cross transversally (shared endpoints / collinear
/// touches don't count — they're the expected case for a fillet's tangent point sitting on the
/// adjacent straight run).
static bool SegmentsCross2D(const glm::dvec2 &a0, const glm::dvec2 &a1, const glm::dvec2 &b0, const glm::dvec2 &b1)
{
    auto cross = [](const glm::dvec2 &o, const glm::dvec2 &p, const glm::dvec2 &q) -> double
    { return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x); };
    const double d1 = cross(b0, b1, a0);
    const double d2 = cross(b0, b1, a1);
    const double d3 = cross(a0, a1, b0);
    const double d4 = cross(a0, a1, b1);
    return ((d1 > 0.0) != (d2 > 0.0)) && (d1 != 0.0 && d2 != 0.0) &&
           ((d3 > 0.0) != (d4 > 0.0)) && (d3 != 0.0 && d4 != 0.0);
}

/// Shortest distance from `p` to the segment `[a, b]` in 2D.
static double PointSegmentDistance2D(const glm::dvec2 &p, const glm::dvec2 &a, const glm::dvec2 &b)
{
    const glm::dvec2 ab = b - a;
    const double abLen2 = glm::dot(ab, ab);
    if (abLen2 < 1e-18)
        return glm::distance(p, a);
    const double t = glm::clamp(glm::dot(p - a, ab) / abLen2, 0.0, 1.0);
    return glm::distance(p, a + ab * t);
}

static std::vector<glm::dvec3> FilletRingCorners2D(const std::vector<glm::dvec3> &ring, double radiusMm,
                                                    double chordTolMm)
{
    if (ring.size() < 3 || !(radiusMm > 0.0))
        return ring;

    constexpr double kMinTurnDeg = 8.0;
    const double kMinTurnRad = kMinTurnDeg * M_PI / 180.0;
    const double z = ring.front().z;
    const size_t n = ring.size();

    std::vector<glm::dvec2> pts(n);
    for (size_t i = 0; i < n; ++i)
        pts[i] = glm::dvec2(ring[i].x, ring[i].y);

    // Cumulative arc length to each vertex (for measuring "distance along the path" between any
    // two vertices, wrapping around the closed ring) — used by the local-clearance check below.
    std::vector<double> cumLen(n, 0.0);
    double ringLen = 0.0;
    for (size_t k = 0; k < n; ++k)
    {
        cumLen[k] = ringLen;
        ringLen += glm::distance(pts[k], pts[(k + 1) % n]);
    }
    auto pathDistance = [&](size_t a, size_t b) -> double
    {
        const double d = std::abs(cumLen[a] - cumLen[b]);
        return std::min(d, ringLen - d);
    };

    // Classify every vertex up front: a true CAD corner turns sharply between its neighbours;
    // discretization noise within one smooth curve does not (chordTolMm keeps consecutive-sample
    // turns tiny, well under `kMinTurnDeg`). The trim cap below then measures the run to the
    // *nearest other corner* — not the immediate sample spacing — so a corner that happens to sit
    // where the curve is finely tessellated (e.g. high local curvature right next to it) still
    // gets the full requested radius as long as the actual edge run leading to it is long enough.
    std::vector<bool> isCorner(n, false);
    for (size_t i = 0; i < n; ++i)
    {
        const glm::dvec2 &prev = pts[(i + n - 1) % n];
        const glm::dvec2 &cur = pts[i];
        const glm::dvec2 &next = pts[(i + 1) % n];
        const glm::dvec2 inDir = cur - prev;
        const glm::dvec2 outDir = next - cur;
        const double inLen = glm::length(inDir);
        const double outLen = glm::length(outDir);
        if (inLen < 1e-9 || outLen < 1e-9)
            continue;
        const double cosTurn = glm::clamp(glm::dot(inDir / inLen, outDir / outLen), -1.0, 1.0);
        isCorner[i] = std::acos(cosTurn) >= kMinTurnRad;
    }

    // Arc-length run from `start` to the nearest other corner in `step` direction (±1), not
    // including `start` itself. Caps how far a fillet can trim back without overlapping its
    // neighbour's.
    auto runToNextCorner = [&](size_t start, int step) -> double
    {
        double acc = 0.0;
        size_t i = start;
        for (size_t steps = 0; steps < n; ++steps)
        {
            const size_t j = (step > 0) ? (i + 1) % n : (i + n - 1) % n;
            acc += glm::distance(pts[i], pts[j]);
            if (isCorner[j])
                break;
            i = j;
        }
        return acc;
    };

    // Start emission in the middle of the longest run of non-corner vertices. The output ring is
    // closed, so the start point is geometrically arbitrary — but the sequential neighbour check
    // below only looks *backward* along emission order (it has no fillet to compare the very
    // first one against). Anchoring the seam in the middle of the longest smooth stretch all but
    // guarantees the corners immediately before and after the seam are far apart, so the missing
    // backward check at the seam never matters in practice.
    size_t rotStart = 0;
    {
        size_t bestRun = 0;
        for (size_t start = 0; start < n; ++start)
        {
            if (isCorner[start] || (start > 0 && !isCorner[start - 1]))
                continue;
            size_t run = 0;
            size_t k = start;
            while (run < n && !isCorner[k])
            {
                ++run;
                k = (k + 1) % n;
            }
            if (run > bestRun)
            {
                bestRun = run;
                rotStart = (start + run / 2) % n;
            }
        }
    }

    std::vector<glm::dvec3> out;
    out.reserve(n * 2);
    size_t prevFilletOutStart = SIZE_MAX;
    for (size_t step = 0; step < n; ++step)
    {
        const size_t i = (rotStart + step) % n;
        if (!isCorner[i])
        {
            out.push_back(ring[i]);
            continue;
        }

        const glm::dvec2 &prev = pts[(i + n - 1) % n];
        const glm::dvec2 &cur = pts[i];
        const glm::dvec2 &next = pts[(i + 1) % n];
        const glm::dvec2 inVec = cur - prev;
        const glm::dvec2 outVec = next - cur;
        const double inLen = glm::length(inVec);
        const double outLen = glm::length(outVec);
        const glm::dvec2 inN = inVec / inLen;
        const glm::dvec2 outN = outVec / outLen;

        const double turn = std::acos(glm::clamp(glm::dot(inN, outN), -1.0, 1.0));
        const double halfTurn = turn * 0.5;
        const double tanHalf = std::tan(halfTurn);
        const double sinHalf = std::sin(halfTurn);
        if (tanHalf < 1e-9 || sinHalf < 1e-9)
        {
            out.push_back(ring[i]);
            continue;
        }

        // Cap by the run to each neighbouring corner (halved, so two adjacent fillets can't
        // overlap) rather than the immediate sample spacing — see the classification note above.
        const double runBack = runToNextCorner(i, -1);
        const double runFwd = runToNextCorner(i, +1);
        // Hard correctness floor: `p0`/`p1` are placed by *linearly extrapolating* from the corner
        // along the immediate edge directions (`inN`/`outN`), so they only stay ON the original
        // curve while `trim` doesn't exceed the immediate segment length. Exceeding it makes `p0`/
        // `p1` overshoot past `prev`/`next` into empty space along that linear extrapolation —
        // and the bridging segment from the previous straight-run point to the (now off-curve) p0
        // can cut clean across the curve, producing exactly the spike-through-the-arc artifact.
        // The run-to-next-corner cap doesn't catch this because it sums *arc length* over many
        // tessellation segments, which can be far larger than any single one of them.
        double trim = radiusMm * tanHalf;
        trim = std::min({trim, inLen, outLen, runBack * 0.5, runFwd * 0.5});
        if (trim < 1e-6)
        {
            out.push_back(ring[i]);
            continue;
        }

        // Local clearance: the run-to-next-corner cap above only guards against overlap measured
        // *along the path*. A thin sliver/notch can fold the ring back so that geometry which is
        // far away along the path sits spatially close to this corner — left unchecked, a large
        // arc reaches across and crosses it (the bowtie/X artifact). Ignore the stretch within
        // `trim` arc-length on either side (that's the part this fillet itself consumes — it's
        // *supposed* to be close) and measure the nearest approach of everything else. The arc's
        // farthest point from the corner is `effRadius * (1 + 1/cosHalf)`, which in terms of trim
        // works out to `trim * (1 + cosHalf) / sinHalf = trim / tan(turn / 4)`; solving that
        // bound against `clearance` gives the cap below.
        double clearance = std::numeric_limits<double>::infinity();
        for (size_t j = 0; j < n; ++j)
        {
            const size_t j2 = (j + 1) % n;
            if (pathDistance(i, j) <= trim || pathDistance(i, j2) <= trim)
                continue;
            clearance = std::min(clearance, PointSegmentDistance2D(cur, pts[j], pts[j2]));
        }
        if (std::isfinite(clearance))
            trim = std::min(trim, clearance * std::tan(turn * 0.25));
        if (trim < 1e-6)
        {
            out.push_back(ring[i]);
            continue;
        }
        // Builds the candidate fillet polyline (tangent point → arc → tangent point) for a given
        // trim distance. False on degenerate bisector (180°-opposite tangent directions).
        auto buildArc = [&](double t, std::vector<glm::dvec3> &arcPts) -> bool
        {
            const double r = t / tanHalf;
            const double cd = t / sinHalf;
            const glm::dvec2 q0 = cur - inN * t;
            const glm::dvec2 q1 = cur + outN * t;
            glm::dvec2 bis = outN - inN;
            const double bisLen = glm::length(bis);
            if (bisLen < 1e-9)
                return false;
            bis /= bisLen;
            const glm::dvec2 c = cur + bis * cd;
            const double aa0 = std::atan2(q0.y - c.y, q0.x - c.x);
            const double aa1 = std::atan2(q1.y - c.y, q1.x - c.x);
            double sw = aa1 - aa0;
            if (sw > M_PI) sw -= 2.0 * M_PI;
            if (sw < -M_PI) sw += 2.0 * M_PI;
            const int segs = std::max(2, static_cast<int>(std::ceil(std::abs(sw) * r /
                                                                     std::max(chordTolMm, 1e-3))));
            arcPts.clear();
            arcPts.reserve(static_cast<size_t>(segs) + 1);
            arcPts.push_back(glm::dvec3(q0.x, q0.y, z));
            for (int s = 1; s < segs; ++s)
            {
                const double tt = static_cast<double>(s) / static_cast<double>(segs);
                const double a = aa0 + sw * tt;
                arcPts.push_back(glm::dvec3(c.x + r * std::cos(a), c.y + r * std::sin(a), z));
            }
            arcPts.push_back(glm::dvec3(q1.x, q1.y, z));
            return true;
        };

        std::vector<glm::dvec3> arcPts;
        bool arcOk = buildArc(trim, arcPts);

        // Sequential consistency: the clearance check above only guards against geometry that's
        // far away along the path — it deliberately ignores the stretch within `trim` of this
        // corner, since that's what *this* fillet consumes. But that's exactly where the
        // *previous* corner's fillet lives. Two corners on an S-bend/inflection can each pass
        // their own checks yet still bulge toward each other and cross the short straight run (or
        // each other's arc) between them. Check the candidate arc against everything emitted
        // since the previous fillet started; shrink and retry on collision, falling back to a
        // sharp corner if even a minimal radius still collides.
        if (arcOk && prevFilletOutStart != SIZE_MAX)
        {
            for (int attempt = 0; attempt < 8; ++attempt)
            {
                bool collides = false;
                for (size_t k = prevFilletOutStart; k + 1 < out.size() && !collides; ++k)
                {
                    const glm::dvec2 b0(out[k].x, out[k].y);
                    const glm::dvec2 b1(out[k + 1].x, out[k + 1].y);
                    for (size_t m = 0; m + 1 < arcPts.size(); ++m)
                    {
                        if (SegmentsCross2D(glm::dvec2(arcPts[m].x, arcPts[m].y),
                                            glm::dvec2(arcPts[m + 1].x, arcPts[m + 1].y), b0, b1))
                        {
                            collides = true;
                            break;
                        }
                    }
                }
                if (!collides)
                    break;
                trim *= 0.6;
                if (trim < 1e-6)
                {
                    arcOk = false;
                    break;
                }
                arcOk = buildArc(trim, arcPts);
            }
        }

        if (!arcOk)
        {
            out.push_back(ring[i]);
            continue;
        }

        prevFilletOutStart = out.size();
        for (const glm::dvec3 &p : arcPts)
            out.push_back(p);
    }
    return out;
}

static std::vector<std::vector<glm::dvec3>> FilletRings2D(const std::vector<std::vector<glm::dvec3>> &rings,
                                                           double radiusMm, double chordTolMm)
{
    std::vector<std::vector<glm::dvec3>> out;
    out.reserve(rings.size());
    for (const auto &ring : rings)
        out.push_back(FilletRingCorners2D(ring, radiusMm, chordTolMm));
    return out;
}

/// Result of cutting the fused, squared-off strut quads out of the cut outline exactly once:
/// `notchedRings` is the full footprint (outer + surviving holes + new strut notches);
/// `strutNotchRings` is just the subset of new holes the strut quads carved — i.e. the boundary
/// the rail preview should trace, expressed as the literal same vertex data as the notch in
/// `notchedRings` (not a separate re-tessellation of `fusedStruts`).
struct NotchedOutlineWithStruts
{
    std::vector<std::vector<glm::dvec3>> notchedRings;
    std::vector<std::vector<glm::dvec3>> strutNotchRings;
};

/// Performs the strut-notch boolean cut exactly once and splits the resulting hole rings into
/// "strut notches" (new holes carved by the fused strut quads) vs. surviving original holes, by
/// `RingCentroidXy` comparison against `baseRings`' pre-cut holes — an original hole's centroid
/// is untouched by the cut (struts never cross hole boundaries), so it survives within
/// discretization noise; a strut notch's centroid has no such match. Both `BuildCutOutlinePreviewLines`
/// (consumes `.notchedRings`) and `BuildStrutRailPreviewLines` (consumes `.strutNotchRings`) derive
/// their displayed boundaries from this single extraction, so the strut-quad edges and the notch
/// they carve are bit-identical in XY — eliminating the seam that independently re-tessellating
/// "the same" edge across two different OCCT shapes would otherwise leave behind.
static NotchedOutlineWithStruts ComputeNotchedOutlineWithStruts(
    const Face *face, const std::vector<std::vector<glm::dvec3>> &baseRings, const BakeParams &params)
{
    NotchedOutlineWithStruts result;
    result.notchedRings = baseRings;
    if (face == nullptr || baseRings.empty() || baseRings.front().size() < 3)
        return result;

    const double zBase = baseRings.front().front().z;
    const std::vector<glm::dvec3> outer = baseRings.front();
    const std::vector<std::vector<glm::dvec3>> holes(baseRings.begin() + 1, baseRings.end());

    TopoDS_Face footprintFace = MakeFlatFaceFromOuterAndHoles(outer, holes, zBase);
    if (footprintFace.IsNull())
        return result;

    const TopoDS_Shape fusedStruts = BuildFusedStrutQuadsShapeAtZ(face, params, zBase);
    if (fusedStruts.IsNull())
        return result;

    TopoDS_Shape footprint = footprintFace;
    BRepAlgoAPI_Cut cut(footprint, fusedStruts);
    cut.SetFuzzyValue(1e-3);
    cut.Build();
    if (!cut.IsDone() || cut.Shape().IsNull())
        return result;
    footprint = cut.Shape();

    std::vector<std::vector<glm::dvec3>> cutRings =
        ExtractOffsetRingsFromFootprint(footprint, params.chordTolMm, zBase);
    if (cutRings.empty())
        return result;

    // Identify which holes are new strut notches vs. surviving original holes *before* rounding —
    // rounding nudges a ring's centroid slightly (more so for small/asymmetric holes), which would
    // make the match against `holes`' pre-cut centroids less reliable.
    constexpr double kCentroidEps = 0.05;
    std::vector<bool> isStrutNotch(cutRings.size(), false);
    for (size_t i = 1; i < cutRings.size(); ++i)
    {
        const glm::dvec2 c = RingCentroidXy(cutRings[i]);
        bool matchesOriginalHole = false;
        for (const auto &hole : holes)
        {
            if (glm::distance(c, RingCentroidXy(hole)) <= kCentroidEps)
            {
                matchesOriginalHole = true;
                break;
            }
        }
        isStrutNotch[i] = !matchesOriginalHole;
    }

    // Round every corner — outer outline, surviving holes, and new strut notches alike — to
    // `insetMm` (the 1:1 fillet-radius convention used elsewhere in Structure). One rounding pass
    // over the shared cut rings, split by the indices already determined above, keeps the rounded
    // strut-notch boundary bit-identical to its counterpart in the full outline — same "share one
    // source" guarantee the seam fix relies on, just carried one step further.
    std::vector<std::vector<glm::dvec3>> filletedRings =
        FilletRings2D(cutRings, params.insetMm, params.chordTolMm);

    std::vector<std::vector<glm::dvec3>> strutNotches;
    for (size_t i = 1; i < filletedRings.size(); ++i)
        if (isStrutNotch[i])
            strutNotches.push_back(filletedRings[i]);

    result.notchedRings = std::move(filletedRings);
    result.strutNotchRings = std::move(strutNotches);
    return result;
}

} // namespace

// Change B of the curve-preserving rewrite: notch the fused, squared-off strut quads out of the
// curve-walled carve footprint, keeping its analytic edges. Reuses `BuildFusedStrutQuadsShapeAtZ`
// (the same OCCT-boolean strut geometry the polygon path and the preview rails already share) and
// subtracts it from the curved footprint with one `BRepAlgoAPI_Cut` — no discretization. The
// footprint comes in at z=0 (built by `BuildCurvedCarveFootprintMvp`), so the struts are stamped on
// the same plane for a clean coplanar cut. Returns the footprint unchanged when the face has no
// struts, and falls back to the un-notched footprint (never null) if the cut fails or invalidates,
// so a strut-notch problem degrades gracefully rather than dropping the whole curved path.
namespace
{

/// Splits a notched footprint into its disjoint void pockets and fills any pocket back to solid
/// whose own perimeter-driven wall cost would outweigh its own area, instead of carving it away.
/// Struts are never touched here — every selected strut stays exactly as chosen; this only decides
/// which of the void regions struts split the cavity into are worth printing as void at all.
/// Returns `notched` unchanged when there's nothing to filter (one piece, or every piece passes).
///
/// `notched` lives in the flattened working plane the vertical-prism carve pipeline uses (see
/// `BuildCurvedCarveFootprintMvp`'s "project first, then offset" shear) — exact only along the
/// face's level axis, foreshortened by cos(tilt) along its slope axis. Measuring area/perimeter
/// there directly would make the same pocket pass on a flat face and fail once tilted, so each
/// candidate is un-flattened back onto the face's real plane (the algebraic inverse of that same
/// shear) before measuring, making the test orientation-independent.
TopoDS_Shape FilterUnworthyVoidPockets(const TopoDS_Shape &notched, double wallThicknessMm, const Face *face)
{
    std::vector<TopoDS_Face> faces;
    for (TopExp_Explorer exp(notched, TopAbs_FACE); exp.More(); exp.Next())
        faces.push_back(TopoDS::Face(exp.Current()));
    if (faces.size() <= 1)
        return notched;

    gp_GTrsf unflatten;
    bool haveUnflatten = false;
    if (face != nullptr)
    {
        const FacePlaneFrame frame = MakeFacePlaneFrame(face);
        glm::dvec3 n = frame.n;
        const double nLen = glm::length(n);
        if (nLen > 1e-9)
        {
            n /= nLen;
            if (std::abs(n.z) > 1e-6)
            {
                const double planeD = glm::dot(n, frame.origin);
                unflatten.SetValue(1, 1, 1.0);      unflatten.SetValue(1, 2, 0.0);      unflatten.SetValue(1, 3, 0.0); unflatten.SetValue(1, 4, 0.0);
                unflatten.SetValue(2, 1, 0.0);      unflatten.SetValue(2, 2, 1.0);      unflatten.SetValue(2, 3, 0.0); unflatten.SetValue(2, 4, 0.0);
                unflatten.SetValue(3, 1, -n.x / n.z); unflatten.SetValue(3, 2, -n.y / n.z); unflatten.SetValue(3, 3, 1.0); unflatten.SetValue(3, 4, planeD / n.z);
                haveUnflatten = true;
            }
        }
    }

    const bool dbgVerboseEarly = std::getenv("CAD_STRUT_RAW_DUMP") != nullptr;
    auto trueFace = [&](const TopoDS_Face &f) -> TopoDS_Face
    {
        if (!haveUnflatten)
            return f;
        try
        {
            const TopoDS_Shape s = BRepBuilderAPI_GTransform(f, unflatten, Standard_True).Shape();
            TopExp_Explorer fexp(s, TopAbs_FACE);
            if (fexp.More())
                return TopoDS::Face(fexp.Current());
            if (dbgVerboseEarly)
                std::cerr << "[strutvoid] face=" << face << " unflatten produced no face, falling back to flat\n";
        }
        catch (const Standard_Failure &e)
        {
            if (dbgVerboseEarly)
                std::cerr << "[strutvoid] face=" << face << " unflatten GTransform threw: " << e.GetMessageString() << "\n";
        }
        return f; // fall back to the (foreshortened) flat measurement rather than failing outright
    };

    auto faceArea = [](const TopoDS_Face &f) -> double
    {
        GProp_GProps props;
        BRepGProp::SurfaceProperties(f, props);
        return props.Mass();
    };
    auto facePerimeter = [](const TopoDS_Face &f) -> double
    {
        double total = 0.0;
        for (TopExp_Explorer eexp(f, TopAbs_EDGE); eexp.More(); eexp.Next())
        {
            GProp_GProps props;
            BRepGProp::LinearProperties(eexp.Current(), props);
            total += props.Mass();
        }
        return total;
    };

    const bool dbgVerbose = std::getenv("CAD_STRUT_RAW_DUMP") != nullptr;
    std::vector<TopoDS_Face> kept;
    std::vector<double> dbgAreas;
    for (const TopoDS_Face &f : faces)
    {
        const TopoDS_Face measured = trueFace(f);
        const double area = faceArea(measured);
        const double wallCost = facePerimeter(measured) * wallThicknessMm;
        const bool passes = wallCost < area;
        if (dbgVerbose)
        {
            const double flatArea = faceArea(f);
            const double flatPerim = facePerimeter(f);
            std::cerr << "[strutvoid] face=" << face << " haveUnflatten=" << haveUnflatten
                      << " flatArea=" << flatArea << " flatPerim=" << flatPerim
                      << " trueArea=" << area << " truePerim=" << (wallCost / wallThicknessMm)
                      << " wallCost=" << wallCost << " -> " << (passes ? "keep" : "fill") << "\n";
            dbgAreas.push_back(area);
        }
        if (passes)
            kept.push_back(f); // keep the ORIGINAL flattened face — that's what feeds the cut/extrude pipeline
    }
    if (dbgVerbose)
    {
        std::cerr << "[strutvoid] face=" << face << " pockets=" << faces.size()
                  << " kept=" << kept.size() << "\n";
        std::sort(dbgAreas.begin(), dbgAreas.end());
        std::cerr << "[strutareas] face=" << face << " n=" << dbgAreas.size() << " areas=";
        for (size_t i = 0; i < dbgAreas.size(); ++i)
            std::cerr << (i == 0 ? "" : ",") << dbgAreas[i];
        std::cerr << "\n";
    }
    if (kept.size() == faces.size())
        return notched; // every pocket earns its wall cost — nothing to fill in

    if (kept.empty())
    {
        // Every pocket failed. Keep the single largest one rather than collapsing to an empty
        // shape: an empty footprint here would fall through to the legacy polygon carve path
        // (which doesn't know about this filter) instead of simply skipping the carve.
        auto best = std::max_element(faces.begin(), faces.end(),
                                     [&](const TopoDS_Face &a, const TopoDS_Face &b)
                                     { return faceArea(trueFace(a)) < faceArea(trueFace(b)); });
        kept.push_back(*best);
    }

    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    for (const TopoDS_Face &f : kept)
        builder.Add(compound, f);
    return compound;
}

} // namespace

TopoDS_Shape NotchStrutsIntoCurvedFootprint(const Face *face, const BakeParams &params,
                                            const TopoDS_Shape &curvedFootprint)
{
    if (curvedFootprint.IsNull())
        return curvedFootprint;

    // Stamp the struts on the footprint's own plane (its Z), not a hardcoded z=0 — the world-frame
    // curved footprint sits at the face's world height, so the struts must be coplanar with it for
    // the notch boolean to bite.
    Bnd_Box fpBox;
    BRepBndLib::Add(curvedFootprint, fpBox);
    double footprintZ = 0.0;
    if (!fpBox.IsVoid())
    {
        double bx0, by0, bz0, bx1, by1, bz1;
        fpBox.Get(bx0, by0, bz0, bx1, by1, bz1);
        footprintZ = 0.5 * (bz0 + bz1);
    }
    // Skip the polygon-footprint clip — the curved footprint itself is the clip tool.
    // BRepAlgoAPI_Cut below trims at the exact OCCT arc edges without polygon discretization.
    const TopoDS_Shape fusedStruts = BuildFusedStrutQuadsShapeAtZ(face, params, footprintZ, /*skipFootprintClip=*/true);
    if (fusedStruts.IsNull())
        return curvedFootprint; // no struts on this face — nothing to notch

    BRepAlgoAPI_Cut cut(curvedFootprint, fusedStruts);
    cut.SetFuzzyValue(1e-3);
    cut.Build();
    if (!cut.IsDone() || cut.Shape().IsNull())
        return curvedFootprint; // notch failed — keep the un-notched footprint
    if (!ValidateOutlineShapeTopology(cut.Shape(), "NotchStrutsIntoCurvedFootprint"))
        return curvedFootprint; // notch produced invalid topology — keep un-notched

    if (!params.weightAwareStrutFilter)
        return cut.Shape();
    return FilterUnworthyVoidPockets(cut.Shape(), params.wallThicknessMm, face);
}


std::vector<std::pair<glm::vec3, glm::vec3>> BuildStrutRailPreviewLines(const Face *face,
                                                                        const BakeParams &params)
{
    std::vector<std::pair<glm::vec3, glm::vec3>> out;
    if (face == nullptr || !(params.insetMm > 0.0))
        return out;

    const auto strutSegments = BuildStrutPreviewLines(face, params);
    if (strutSegments.empty())
        return out;

    // All strut geometry for this face shares one Z (the preview plane `BuildStrutSetup` derived
    // from the outline rings, lifted slightly off the face) — any endpoint's Z will do.
    const double zPlane = static_cast<double>(strutSegments.front().first.z);

    std::vector<std::vector<glm::dvec3>> baseRings =
        StructurePreviewUsesXyCarveFootprint() ? BuildXyCarveFootprintPreviewRings(face, params)
                                               : BuildTrimmedFacePlanePreviewRings(face, params);
    if (baseRings.empty() || baseRings.front().size() < 3)
        return out;
    const double zBase = baseRings.front().front().z;

    const NotchedOutlineWithStruts notched = ComputeNotchedOutlineWithStruts(face, baseRings, params);
    if (notched.strutNotchRings.empty())
        return out;

    // Translate the shared notch boundary up to the rail preview's lifted Z — pure translation,
    // so the XY vertex data stays bit-identical to the cut-outline notch and the two boundaries
    // trace exactly the same line (no seam from independently re-tessellating the same edge).
    return RingsToPreviewLineSegments(notched.strutNotchRings, glm::dvec3(0.0, 0.0, zPlane - zBase));
}

TopoDS_Shape ExtrudeFlattenedFootprint(const TopoDS_Shape &footprintOnFacePlane, double zBottom, double zTop,
                                       double chordTolMm, double minSpanMm,
                                       const glm::dvec3 &normalHint)
{
    (void)normalHint;
    if (footprintOnFacePlane.IsNull() || !(zBottom < zTop))
        return TopoDS_Shape();

    const TopoDS_Shape flatFootprint =
        BuildCarveCompoundFromFootprint(footprintOnFacePlane, zBottom, chordTolMm, minSpanMm);
    if (flatFootprint.IsNull())
        return TopoDS_Shape();

    BRepPrimAPI_MakePrism prismMaker(flatFootprint, gp_Vec(0, 0, zTop - zBottom));
    const TopoDS_Shape prism = prismMaker.Shape();
    ValidateOutlineShapeTopology(prism, "ExtrudeFlattenedFootprint");
    return prism;
}

void AppendPreviewLineSegments(const std::vector<std::pair<glm::vec3, glm::vec3>> &src,
                               std::vector<std::pair<glm::vec3, glm::vec3>> &dst)
{
    dst.insert(dst.end(), src.begin(), src.end());
}

std::vector<std::pair<glm::vec3, glm::vec3>> RingsToPreviewLineSegments(
    const std::vector<std::vector<glm::dvec3>> &rings, const glm::dvec3 &previewLift)
{
    std::vector<std::pair<glm::vec3, glm::vec3>> segments;
    for (const auto &ring : rings)
    {
        if (ring.size() < 3)
            continue;
        for (size_t i = 0; i < ring.size(); ++i)
        {
            const size_t j = (i + 1) % ring.size();
            const glm::dvec3 a = ring[i] + previewLift;
            const glm::dvec3 b = ring[j] + previewLift;
            segments.push_back({glm::vec3(a.x, a.y, a.z), glm::vec3(b.x, b.y, b.z)});
        }
    }
    return segments;
}

/// Notches the fused, squared-off strut quads out of the cut-outline `rings` (closed XY loops
/// sharing a common Z plane) via `ComputeNotchedOutlineWithStruts` — the single shared cut that
/// also feeds `BuildStrutRailPreviewLines`, so the notch boundary and the rail edges are bit-
/// identical in XY (no seam). Mirrors the carve's `ApplyStrutsToFootprint` so the previewed
/// outline matches what the carve will actually cut. Returns `rings` unchanged if there are no
/// struts, or any rebuild/boolean step fails.
std::vector<std::vector<glm::dvec3>> ApplyStrutCutoutsToOutlineRings(
    const Face *face, std::vector<std::vector<glm::dvec3>> rings, const BakeParams &params)
{
    if (face == nullptr || rings.empty() || rings.front().size() < 3)
        return rings;

    NotchedOutlineWithStruts notched = ComputeNotchedOutlineWithStruts(face, rings, params);
    return notched.notchedRings.empty() ? rings : std::move(notched.notchedRings);
}

TopoDS_Shape BuildNotchedCarveFootprint(const Face *face, const BakeParams &params)
{
    if (face == nullptr)
        return TopoDS_Shape();

    std::vector<std::vector<glm::dvec3>> rings =
        StructurePreviewUsesXyCarveFootprint() ? BuildXyCarveFootprintPreviewRings(face, params)
                                               : BuildTrimmedFacePlanePreviewRings(face, params);
    if (rings.empty() || rings.front().size() < 3)
        return TopoDS_Shape();

    rings = ApplyStrutCutoutsToOutlineRings(face, std::move(rings), params);
    if (rings.empty() || rings.front().size() < 3)
        return TopoDS_Shape();

    const double zBase = rings.front().front().z;
    const std::vector<glm::dvec3> outer = rings.front();
    const std::vector<std::vector<glm::dvec3>> holes(rings.begin() + 1, rings.end());
    TopoDS_Shape outShape = MakeFlatFaceFromOuterAndHoles(outer, holes, zBase);
    return outShape;
}

// Preview-only verification of the curve-native cut outline (no carve changes). Mirrors
// `BuildTrimmedXyProjectedPreviewRings`'s frame exactly: offsets/trims are computed in
// face-local-flat (where analytic arc/ellipse edges survive), then the curved footprint SHAPE is
// mapped to world with the same rigid `toFlat.Inverted()` the ring path applies — so the curve
// outline overlays the model identically to the existing (discretized) preview, but stays curved.
// Lets us confirm the curve-native outline is correct before it ever drives a cut. Returns empty on
// any failure (or >2 holes, which upstream handles with a non-rigid polygon map this can't mirror),
// so the caller falls back to the existing polygon preview. Struts/fillets not added yet.
std::vector<std::pair<glm::vec3, glm::vec3>> BuildCurvedCutOutlinePreviewLines(const Face *face,
                                                                              const BakeParams &params)
{
    std::vector<std::pair<glm::vec3, glm::vec3>> segments;
    if (face == nullptr)
        return segments;

    // The carve and the preview now share ONE world-frame curved footprint builder, so the overlay
    // shows exactly the shape the cut would extrude (struts/fillets still pending).
    const TopoDS_Shape worldShape = BuildCurvedCarveFootprintMvp(face, params);
    if (worldShape.IsNull())
        return segments;

    glm::dvec3 faceNormal{0.0, 0.0, 1.0};
    if (face->surface != nullptr)
    {
        faceNormal = face->surface->GetNormal();
        const double nLen = glm::length(faceNormal);
        if (nLen > 1e-12)
            faceNormal /= nLen;
    }

    // Display-only tessellation of the curved wires, kept in 3D world (no z flattening), lifted
    // slightly off the face so the outline is visible over the cap.
    const glm::dvec3 lift = faceNormal * 0.06;
    for (TopExp_Explorer exWire(worldShape, TopAbs_WIRE); exWire.More(); exWire.Next())
    {
        const std::vector<glm::dvec3> ring =
            ExtractRingPointsInWireOrder(TopoDS::Wire(exWire.Current()), params.chordTolMm);
        if (ring.size() < 3)
            continue;
        for (size_t i = 0; i < ring.size(); ++i)
        {
            const glm::dvec3 a = ring[i] + lift;
            const glm::dvec3 b = ring[(i + 1) % ring.size()] + lift;
            segments.push_back({glm::vec3(a.x, a.y, a.z), glm::vec3(b.x, b.y, b.z)});
        }
    }
    return segments;
}

std::vector<std::pair<glm::vec3, glm::vec3>> BuildCutOutlinePreviewLines(const Face *face,
                                                                          const BakeParams &params)
{
    if (face == nullptr)
        return {};
    const CacheKey key{face,           params.insetMm,
                       params.chordTolMm, params.minFeatureMm,
                       params.weightAwareStrutFilter, params.wallThicknessMm};
    auto &cache = LineBakeCache();
    const bool stepMode = StructureDebugStepCutMode();
    if (!stepMode)
    {
        if (auto it = cache.find(key); it != cache.end())
            return it->second;
    }

    // Curve-native cut-outline overlay (default): show the same curved outline the carve extrudes,
    // so the preview matches the cut. Falls back to the polygon preview below when empty (e.g. a
    // build failure or a 3+-hole face, which the curve path doesn't cover).
    if (StructureCurvedCarveEnabled())
    {
        std::vector<std::pair<glm::vec3, glm::vec3>> curvedSegments =
            BuildCurvedCutOutlinePreviewLines(face, params);
        if (!curvedSegments.empty())
        {
            if (!stepMode)
                cache.emplace(key, curvedSegments);
            return curvedSegments;
        }
    }

    std::vector<std::vector<glm::dvec3>> rings =
        StructurePreviewUsesXyCarveFootprint() ? BuildXyCarveFootprintPreviewRings(face, params)
                                               : BuildTrimmedFacePlanePreviewRings(face, params);
    // Notch the fused strut quads out of the outline so the preview matches what the carve will
    // actually cut (struts are preserved as material, not removed with the rest of the footprint).
    // Shares its strut geometry with `BuildStrutRailPreviewLines` via `BuildFusedStrutQuadsShapeAtZ`,
    // so the notch boundary and the rail edges always agree.
    rings = ApplyStrutCutoutsToOutlineRings(face, std::move(rings), params);
    glm::dvec3 basisNormal{0.0, 0.0, 1.0};
    if (face->surface != nullptr)
        basisNormal = face->surface->GetNormal();
    const double nLen = glm::length(basisNormal);
    if (nLen > 1e-12)
        basisNormal /= nLen;

    glm::dvec3 previewLift{0.0, 0.0, 0.0};
    if (StructurePreviewDrawsCarveFootprintInWorldXy())
        previewLift = glm::dvec3(0.0, 0.0, 0.05);
    else if (nLen > 1e-12)
        previewLift = basisNormal * 0.05;

    std::vector<std::pair<glm::vec3, glm::vec3>> segments = RingsToPreviewLineSegments(rings, previewLift);
    const bool debug = StructureDebugEnabled();
    if (segments.empty() && debug && !StructureStopsAtCutOutline())
    {
        const std::vector<std::vector<glm::dvec3>> rawRings =
            BuildFacePlaneOffsetPreviewRings(face, params);
        segments = RingsToPreviewLineSegments(rawRings, previewLift);
    }
    if (segments.empty() && debug && StructureStopsAtCutOutline())
    {
        const std::vector<std::vector<glm::dvec3>> rawRings =
            BuildFacePlaneOffsetPreviewRings(face, params);
        const std::size_t rawSegs = RingsToPreviewLineSegments(rawRings, previewLift).size();
        std::cerr << "Structure cut outline empty (insetMm=" << params.insetMm << " trimmedRings="
                  << rings.size() << " rawRings=" << rawRings.size() << " rawSegs=" << rawSegs
                  << ")\n";
    }

    if (debug)
    {
        const std::vector<std::vector<glm::dvec3>> rawRings =
            BuildFacePlaneOffsetPreviewRings(face, params);
        const std::size_t rawSegs = RingsToPreviewLineSegments(rawRings, previewLift).size();
        std::cerr << "Structure cut outline preview: mode="
                  << (StructurePreviewUsesXyCarveFootprint() ? "xyCarveFootprint" : "facePlaneCut")
                  << " draw="
                  << (StructurePreviewDrawsCarveFootprintInWorldXy() ? "worldXy" : "onCap")
                  << " rings=" << rings.size() << " rawRings=" << rawRings.size()
                  << " segments=" << segments.size() << " rawSegs=" << rawSegs
                  << " insetMm=" << params.insetMm;
        if (stepMode)
            std::cerr << " (step-cut debug — change MAX_CUTS/CUT_ONLY and re-select face)";
        std::cerr << "\n";
    }

    if (!stepMode)
        cache.emplace(key, segments);
    return segments;
}

void ClearBakeCache()
{
    LineBakeCache().clear();
    PreOffsetCache().clear();
    StrutQuadValidityCache().clear();
}

void InvalidateBakeCacheForParams(const BakeParams &params)
{
    auto &cache = LineBakeCache();
    for (auto it = cache.begin(); it != cache.end();)
    {
        if (it->first.insetMm != params.insetMm || it->first.chordTolMm != params.chordTolMm ||
            it->first.minFeatureMm != params.minFeatureMm ||
            it->first.weightAwareStrutFilter != params.weightAwareStrutFilter ||
            it->first.wallThicknessMm != params.wallThicknessMm)
            it = cache.erase(it);
        else
            ++it;
    }
    // Evict pre-offset entries whose geometry params no longer match.
    auto &preCache = PreOffsetCache();
    for (auto it = preCache.begin(); it != preCache.end();)
    {
        if (it->first.chordTolMm != params.chordTolMm || it->first.minFeatureMm != params.minFeatureMm)
            it = preCache.erase(it);
        else
            ++it;
    }
    // Evict strut-quad validity entries whose geometry params no longer match.
    auto &quadCache = StrutQuadValidityCache();
    for (auto it = quadCache.begin(); it != quadCache.end();)
    {
        if (it->first.insetMm != params.insetMm || it->first.chordTolMm != params.chordTolMm)
            it = quadCache.erase(it);
        else
            ++it;
    }
}

std::vector<std::vector<glm::dvec3>> BuildCarveFootprintOuterRingsWorld(
    const Face *face,
    const BakeParams &params,
    const std::function<void(const std::string &)> *workerTrace)
{
    (void)workerTrace;
    return ExtractOffsetRingsFromFootprint(BuildOffsetFootprintOnFace(face, params), params.chordTolMm, 0.0);
}

} // namespace StructureTriangulation
