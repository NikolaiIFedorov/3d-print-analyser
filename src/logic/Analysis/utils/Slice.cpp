#include "Slice.hpp"
#include "scene/Geometry/AllGeometry.hpp"
#include "GeometryOps/RingUtils.hpp"

#include <BRepAlgoAPI_Section.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <limits>
#include <numeric>
#include <unordered_map>

ZBounds Slice::GetZBounds(const Solid *solid)
{
    double zMin = std::numeric_limits<double>::max();
    double zMax = std::numeric_limits<double>::lowest();

    if (solid == nullptr)
        return {zMin, zMax};

    for (const Face *face : solid->faces)
    {
        if (face == nullptr || !face->HasGeometry())
            continue;
        for (const auto &loop : face->loops)
        {
            for (const auto &orientedEdge : loop)
            {
                const Edge *edge = orientedEdge.edge;
                if (edge == nullptr || edge->startPoint == nullptr || edge->endPoint == nullptr)
                    continue;
                zMin = std::min(zMin, edge->startPoint->position.z);
                zMax = std::max(zMax, edge->startPoint->position.z);
                zMin = std::min(zMin, edge->endPoint->position.z);
                zMax = std::max(zMax, edge->endPoint->position.z);
            }
        }
    }

    return {zMin, zMax};
}

// Shortest distance from point p to the line segment (a, b) in XY
static double PointToSegmentDist(const glm::dvec3 &p, const glm::dvec3 &a, const glm::dvec3 &b)
{
    glm::dvec2 ab = glm::dvec2(b) - glm::dvec2(a);
    glm::dvec2 ap = glm::dvec2(p) - glm::dvec2(a);

    double t = glm::dot(ap, ab) / glm::dot(ab, ab);
    t = std::clamp(t, 0.0, 1.0);

    glm::dvec2 closest = glm::dvec2(a) + t * ab;
    return glm::length(glm::dvec2(p) - closest);
}

// Check if two segments share an endpoint (adjacent in the contour). Tolerance must be loose
// enough to weld points computed via different numerical pipelines for the "same" physical
// vertex — e.g. a planar face's edge endpoint (plain linear interpolation) vs. a curved face's
// section-curve endpoint (BRepAlgoAPI_Section + GCPnts_QuasiUniformDeflection tessellation), which
// can disagree by ~1e-9 even though both are geometrically the same point. 1e-10 was too tight and
// silently dropped any loop spanning both a planar and a curved face (the chain fractured into
// disconnected open groups and got discarded). 1e-6 matches the tolerance already trusted
// elsewhere for "same point" (the loop-closing check below, `RemoveConsecutiveDuplicateRingPoints`).
static bool SharesEndpoint(const Segment &s1, const Segment &s2)
{
    const double eps = 1e-6;
    return glm::length(glm::dvec2(s1.a - s2.a)) < eps ||
           glm::length(glm::dvec2(s1.a - s2.b)) < eps ||
           glm::length(glm::dvec2(s1.b - s2.a)) < eps ||
           glm::length(glm::dvec2(s1.b - s2.b)) < eps;
}

std::vector<Segment> Slice::At(const Solid *solid, double z)
{
    std::vector<Segment> segments;
    if (solid == nullptr || solid->occtShape.IsNull())
        return segments;

    // Map from OCCT TShape identity → Face* for edge attribution (face pointer tracking).
    // isHole is determined by winding order in ExtractLoops, not face topology, because
    // face-topology classification misidentifies overhang faces that share a boundary edge
    // with an inner loop of a planar face as hole faces.
    std::unordered_map<const void *, const Face *> tshapeToFace;
    for (const Face *face : solid->faces)
    {
        if (face == nullptr || !face->HasGeometry() || face->occtFace.IsNull())
            continue;
        tshapeToFace[face->occtFace.TShape().get()] = face;
    }

    // Section the entire solid at once. All endpoints are computed by the same OCCT algorithm,
    // so adjacent face sections share endpoints exactly — no numerical pipeline mismatch,
    // no disconnected single-segment groups.
    const gp_Pln plane(gp_Pnt(0.0, 0.0, z), gp_Dir(0.0, 0.0, 1.0));
    BRepAlgoAPI_Section section(solid->occtShape, plane, Standard_False);
    section.ComputePCurveOn1(Standard_False);
    section.Approximation(Standard_True);
    section.Build();
    if (!section.IsDone())
        return segments;

    for (TopExp_Explorer exp(section.Shape(), TopAbs_EDGE); exp.More(); exp.Next())
    {
        const TopoDS_Edge &sectionEdge = TopoDS::Edge(exp.Current());

        const Face *facePtr = nullptr;
        TopoDS_Shape ancestor;
        if (section.HasAncestorFaceOn1(sectionEdge, ancestor) && !ancestor.IsNull())
        {
            auto it = tshapeToFace.find(ancestor.TShape().get());
            if (it != tshapeToFace.end())
                facePtr = it->second;
        }

        std::vector<glm::dvec3> pts;
        GeometryOps::CollectPointsFromEdge(sectionEdge, 0.05, pts);
        for (size_t i = 0; i + 1 < pts.size(); i++)
            segments.push_back({pts[i], pts[i + 1], facePtr, false});
    }

    return segments;
}

std::vector<Layer> Slice::Range(const Solid *solid, double zMin, double zMax, double layerHeight)
{
    std::vector<Layer> layers;

    if (layerHeight <= 0.0)
        return layers;

    for (double z = zMin + layerHeight; z < zMax; z += layerHeight)
    {
        layers.push_back(At(solid, z));
    }

    return layers;
}

double Slice::MinWidth(const std::vector<Segment> &segments)
{
    if (segments.size() < 2)
        return std::numeric_limits<double>::max();

    double minDist = std::numeric_limits<double>::max();

    for (size_t i = 0; i < segments.size(); i++)
    {
        // Sample points along this segment
        const int samples = 8;
        for (int s = 0; s <= samples; s++)
        {
            double t = static_cast<double>(s) / samples;
            glm::dvec3 point = segments[i].a + t * (segments[i].b - segments[i].a);

            // Measure distance to every other non-adjacent segment
            for (size_t j = 0; j < segments.size(); j++)
            {
                if (i == j)
                    continue;

                if (SharesEndpoint(segments[i], segments[j]))
                    continue;

                double dist = PointToSegmentDist(point, segments[j].a, segments[j].b);
                minDist = std::min(minDist, dist);
            }
        }
    }

    return minDist;
}

namespace
{
struct LoopUnionFind
{
    std::vector<int> parent;
    explicit LoopUnionFind(int n) : parent(n) { std::iota(parent.begin(), parent.end(), 0); }
    int find(int x) { return parent[x] == x ? x : parent[x] = find(parent[x]); }
    void unite(int x, int y) { parent[find(x)] = find(y); }
};
} // namespace

std::vector<SliceLoop> Slice::ExtractLoops(const std::vector<Segment> &segments)
{
    if (segments.empty())
        return {};

    const size_t n = segments.size();
    constexpr double eps = 1e-6;

    // Group connected segments via union-find.
    LoopUnionFind uf(static_cast<int>(n));
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (SharesEndpoint(segments[i], segments[j]))
                uf.unite(static_cast<int>(i), static_cast<int>(j));

    std::unordered_map<int, std::vector<size_t>> groups;
    for (size_t i = 0; i < n; i++)
        groups[uf.find(static_cast<int>(i))].push_back(i);

    // First pass: chain-walk all groups into ordered rings without classification.
    struct RawRing { std::vector<glm::dvec3> ring; std::vector<const Face *> edgeFaces; };
    std::vector<RawRing> rawRings;

    for (auto &[root, indices] : groups)
    {
        if (indices.size() < 2)
            continue;

        // Chain-walk the group into an ordered point sequence.
        std::vector<bool> used(indices.size(), false);
        std::vector<glm::dvec3> ring;
        std::vector<const Face *> edgeFaces;
        ring.push_back(segments[indices[0]].a);
        edgeFaces.push_back(segments[indices[0]].face);
        glm::dvec3 current = segments[indices[0]].b;
        used[0] = true;

        while (true)
        {
            int next = -1;
            bool flip = false;
            for (size_t k = 0; k < indices.size(); k++)
            {
                if (used[k])
                    continue;
                const Segment &s = segments[indices[k]];
                if (glm::length(glm::dvec2(s.a - current)) < eps)
                { next = static_cast<int>(k); flip = false; break; }
                if (glm::length(glm::dvec2(s.b - current)) < eps)
                { next = static_cast<int>(k); flip = true; break; }
            }
            if (next == -1)
                break;
            used[next] = true;
            ring.push_back(current);
            edgeFaces.push_back(segments[indices[next]].face);
            current = flip ? segments[indices[next]].a : segments[indices[next]].b;
        }

        // Include the final endpoint for open chains so the chain is complete.
        const bool isClosed = glm::length(glm::dvec2(current - ring.front())) <= eps;
        if (!isClosed)
            ring.push_back(current);

        if (ring.size() < 3)
            continue;

        rawRings.push_back({std::move(ring), std::move(edgeFaces)});
    }

    // Second pass: classify by nesting depth. A loop nested inside an odd number of other
    // loops is a hole; even (including 0) is outer. This is winding-direction-agnostic, which
    // matters because OCCT's section produces CW loops from downward-facing faces (e.g. the
    // underside of an overhang), so signed-area alone misclassifies them.
    auto pointInRing = [](const glm::dvec2 &pt, const std::vector<glm::dvec3> &r) -> bool {
        bool inside = false;
        const size_t m = r.size();
        for (size_t i = 0, j = m - 1; i < m; j = i++)
        {
            const double yi = r[i].y, yj = r[j].y;
            const double xi = r[i].x, xj = r[j].x;
            if (((yi > pt.y) != (yj > pt.y)) &&
                (pt.x < (xj - xi) * (pt.y - yi) / (yj - yi) + xi))
                inside = !inside;
        }
        return inside;
    };

    std::vector<SliceLoop> result;
    result.reserve(rawRings.size());
    for (size_t i = 0; i < rawRings.size(); i++)
    {
        // Use the centroid as the test point (more stable than ring[0] near edges).
        glm::dvec2 centroid{0.0, 0.0};
        for (const auto &p : rawRings[i].ring)
            centroid += glm::dvec2(p.x, p.y);
        centroid /= static_cast<double>(rawRings[i].ring.size());

        int nestDepth = 0;
        for (size_t j = 0; j < rawRings.size(); j++)
        {
            if (i != j && pointInRing(centroid, rawRings[j].ring))
                ++nestDepth;
        }
        const bool isHole = (nestDepth % 2) == 1;
        result.push_back({std::move(rawRings[i].ring), std::move(rawRings[i].edgeFaces), isHole});
    }

    return result;
}
