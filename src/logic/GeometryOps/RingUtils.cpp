#include "GeometryOps/RingUtils.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_CompCurve.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopAbs_ShapeEnum.hxx>

#include <algorithm>
#include <cmath>


namespace GeometryOps
{

glm::dvec3 ProjectPointToXyPlane(const glm::dvec3 &p, double zBottom)
{
    return glm::dvec3(p.x, p.y, zBottom);
}

void BuildPlanarBasis(const glm::dvec3 &normalHint, glm::dvec3 &uOut, glm::dvec3 &vOut)
{
    glm::dvec3 n = normalHint;
    const double nLen = glm::length(n);
    if (nLen > 1e-12)
        n /= nLen;
    else
        n = glm::dvec3(0.0, 0.0, 1.0);
    const glm::dvec3 ref = glm::abs(n.z) < 0.9 ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(0.0, 1.0, 0.0);
    uOut = glm::normalize(glm::cross(ref, n));
    vOut = glm::cross(n, uOut);
}

double SignedArea2D(const std::vector<glm::dvec3> &ring, const glm::dvec3 &normalHint)
{
    if (ring.size() < 3)
        return 0.0;
    glm::dvec3 u;
    glm::dvec3 v;
    BuildPlanarBasis(normalHint, u, v);
    double area = 0.0;
    for (size_t i = 0; i < ring.size(); ++i)
    {
        const size_t j = (i + 1) % ring.size();
        const double xi = glm::dot(ring[i], u);
        const double yi = glm::dot(ring[i], v);
        const double xj = glm::dot(ring[j], u);
        const double yj = glm::dot(ring[j], v);
        area += xi * yj - xj * yi;
    }
    return 0.5 * area;
}

void ClassifyOuterAndHoles(const std::vector<std::vector<glm::dvec3>> &rings, const glm::dvec3 &normalHint,
                           std::vector<glm::dvec3> &outerOut,
                           std::vector<std::vector<glm::dvec3>> &holesOut)
{
    outerOut.clear();
    holesOut.clear();
    if (rings.empty())
        return;
    if (rings.size() == 1)
    {
        outerOut = rings.front();
        if (SignedArea2D(outerOut, normalHint) < 0.0)
            std::reverse(outerOut.begin(), outerOut.end());
        return;
    }

    size_t outerIdx = 0;
    double bestAbsArea = 0.0;
    for (size_t i = 0; i < rings.size(); ++i)
    {
        const double absArea = std::abs(SignedArea2D(rings[i], normalHint));
        if (absArea > bestAbsArea)
        {
            bestAbsArea = absArea;
            outerIdx = i;
        }
    }

    outerOut = rings[outerIdx];
    if (SignedArea2D(outerOut, normalHint) < 0.0)
        std::reverse(outerOut.begin(), outerOut.end());

    for (size_t i = 0; i < rings.size(); ++i)
    {
        if (i == outerIdx)
            continue;
        std::vector<glm::dvec3> hole = rings[i];
        if (SignedArea2D(hole, normalHint) > 0.0)
            std::reverse(hole.begin(), hole.end());
        if (hole.size() >= 3)
            holesOut.push_back(std::move(hole));
    }
}

void AppendRingPoint(std::vector<glm::dvec3> &ring, const gp_Pnt &p)
{
    const glm::dvec3 pt(p.X(), p.Y(), p.Z());
    if (ring.empty() || glm::distance(ring.back(), pt) > 1e-6)
        ring.push_back(pt);
}

void CollectPointsFromEdge(const TopoDS_Edge &edge, double chordTolMm, std::vector<glm::dvec3> &points)
{
    // Use exact topological vertex positions for the first and last point of each edge.
    // GCPnts/BRepAdaptor_Curve evaluate the parametric curve at the boundary parameters,
    // which can differ from the shared vertex position by ~5e-5mm for curved surfaces —
    // enough to break the coordinate-matching chain-walk in ExtractLoops.
    TopoDS_Vertex vStart, vEnd;
    TopExp::Vertices(edge, vStart, vEnd);
    auto vertexPnt = [](const TopoDS_Vertex &v, const gp_Pnt &fallback) -> gp_Pnt {
        return v.IsNull() ? fallback : BRep_Tool::Pnt(v);
    };

    BRepAdaptor_Curve curve(edge);
    if (curve.GetType() == GeomAbs_Line)
    {
        AppendRingPoint(points, vertexPnt(vStart, curve.Value(curve.FirstParameter())));
        AppendRingPoint(points, vertexPnt(vEnd,   curve.Value(curve.LastParameter())));
        return;
    }

    GCPnts_QuasiUniformDeflection disc(curve, chordTolMm);
    if (!disc.IsDone())
    {
        AppendRingPoint(points, vertexPnt(vStart, curve.Value(curve.FirstParameter())));
        AppendRingPoint(points, vertexPnt(vEnd,   curve.Value(curve.LastParameter())));
        return;
    }

    // Replace GCPnts boundary points with exact vertex positions; keep interior points as-is.
    AppendRingPoint(points, vertexPnt(vStart, disc.Value(1)));
    for (int i = 2; i < disc.NbPoints(); ++i)
        AppendRingPoint(points, disc.Value(i));
    if (disc.NbPoints() > 1)
        AppendRingPoint(points, vertexPnt(vEnd, disc.Value(disc.NbPoints())));
}

void RemoveConsecutiveDuplicateRingPoints(std::vector<glm::dvec3> &points, double tol)
{
    if (points.empty())
        return;
    std::vector<glm::dvec3> unique;
    unique.reserve(points.size());
    unique.push_back(points.front());
    for (size_t i = 1; i < points.size(); ++i)
    {
        if (glm::distance(points[i], unique.back()) > tol)
            unique.push_back(points[i]);
    }
    if (unique.size() > 1 && glm::distance(unique.front(), unique.back()) <= tol)
        unique.pop_back();
    points.swap(unique);
}

glm::dvec2 RingCentroidXy(const std::vector<glm::dvec3> &ring)
{
    glm::dvec2 c(0.0);
    if (ring.empty())
        return c;
    for (const glm::dvec3 &p : ring)
    {
        c.x += p.x;
        c.y += p.y;
    }
    c /= static_cast<double>(ring.size());
    return c;
}

double RingSpanXyMm(const std::vector<glm::dvec3> &ring)
{
    if (ring.empty())
        return 0.0;
    double minX = ring.front().x;
    double maxX = ring.front().x;
    double minY = ring.front().y;
    double maxY = ring.front().y;
    for (const glm::dvec3 &p : ring)
    {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    return std::max(maxX - minX, maxY - minY);
}

double RingMaxSpanMm(const std::vector<glm::dvec3> &ring)
{
    if (ring.empty())
        return 0.0;
    glm::dvec3 mn = ring.front();
    glm::dvec3 mx = ring.front();
    for (const glm::dvec3 &p : ring)
    {
        mn = glm::min(mn, p);
        mx = glm::max(mx, p);
    }
    return glm::length(mx - mn);
}

std::vector<glm::dvec3> ExtractRingPointsInWireOrder(const TopoDS_Wire &wire, double chordTolMm)
{
    std::vector<glm::dvec3> points;
    if (wire.IsNull())
        return points;

    int edgeCount = 0;
    for (TopExp_Explorer ex(wire, TopAbs_EDGE); ex.More(); ex.Next())
        ++edgeCount;

    if (edgeCount <= 1)
    {
        for (BRepTools_WireExplorer exEdge(wire); exEdge.More(); exEdge.Next())
            CollectPointsFromEdge(exEdge.Current(), chordTolMm, points);
        RemoveConsecutiveDuplicateRingPoints(points, 1e-6);
        return points;
    }

    // Sample the wire as one continuous curve so segment i→i+1 never jumps across edge joins.
    BRepAdaptor_CompCurve comp(wire);
    GCPnts_QuasiUniformDeflection disc(comp, chordTolMm);
    if (!disc.IsDone())
    {
        for (BRepTools_WireExplorer exEdge(wire); exEdge.More(); exEdge.Next())
            CollectPointsFromEdge(exEdge.Current(), chordTolMm, points);
    }
    else
    {
        for (int i = 1; i <= disc.NbPoints(); ++i)
            AppendRingPoint(points, disc.Value(i));
    }
    RemoveConsecutiveDuplicateRingPoints(points, 1e-6);
    return points;
}

bool PointInRingPlanar(glm::dvec2 p, const std::vector<glm::dvec3> &ring, const glm::dvec3 &u,
                       const glm::dvec3 &v)
{
    if (ring.size() < 3)
        return false;
    bool inside = false;
    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
    {
        const glm::dvec2 a(glm::dot(ring[i], u), glm::dot(ring[i], v));
        const glm::dvec2 b(glm::dot(ring[j], u), glm::dot(ring[j], v));
        const bool intersects = ((a.y > p.y) != (b.y > p.y)) &&
                                (p.x < (b.x - a.x) * (p.y - a.y) / ((b.y - a.y) + 1e-30) + a.x);
        if (intersects)
            inside = !inside;
    }
    return inside;
}

double RingPerimeterMm(const std::vector<glm::dvec3> &ring)
{
    if (ring.size() < 2)
        return 0.0;
    double perimeter = 0.0;
    for (size_t i = 0; i < ring.size(); ++i)
    {
        const size_t j = (i + 1) % ring.size();
        perimeter += glm::length(ring[j] - ring[i]);
    }
    return perimeter;
}

/// 1.0 = circle; lower = more elongated.
double RingRoundness(const std::vector<glm::dvec3> &ring, const glm::dvec3 &basisNormal)
{
    if (ring.size() < 3)
        return 0.0;
    const double area = std::abs(SignedArea2D(ring, basisNormal));
    const double perimeter = RingPerimeterMm(ring);
    if (perimeter <= 1e-9)
        return 0.0;
    return (4.0 * M_PI * area) / (perimeter * perimeter);
}

/// Inner ring index (1..n-1) whose interior contains p; -1 if none.
int FindInnerRingContainingPoint(const glm::dvec2 &p, const std::vector<std::vector<glm::dvec3>> &rings,
                                 const glm::dvec3 &basisNormal)
{
    if (rings.size() < 2)
        return -1;
    glm::dvec3 u;
    glm::dvec3 v;
    BuildPlanarBasis(basisNormal, u, v);
    for (size_t i = 1; i < rings.size(); ++i)
    {
        if (PointInRingPlanar(p, rings[i], u, v))
            return static_cast<int>(i);
    }
    return -1;
}

bool InsideFootprintPlanar(glm::dvec2 p, const std::vector<glm::dvec3> &outerRing,
                           const std::vector<std::vector<glm::dvec3>> &holeRings, const glm::dvec3 &u,
                           const glm::dvec3 &v)
{
    if (!PointInRingPlanar(p, outerRing, u, v))
        return false;
    for (const std::vector<glm::dvec3> &holeRing : holeRings)
    {
        if (PointInRingPlanar(p, holeRing, u, v))
            return false;
    }
    return true;
}

} // namespace GeometryOps
