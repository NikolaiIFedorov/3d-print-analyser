#include "Face.hpp"
#include "utils/log.hpp"

#include <vector>

namespace
{
static glm::dvec3 RobustUnitNormalTriangle(const glm::dvec3 &a, const glm::dvec3 &b, const glm::dvec3 &c)
{
    const glm::dvec3 n0 = glm::cross(b - a, c - a);
    const glm::dvec3 n1 = glm::cross(c - b, a - b);
    const glm::dvec3 n2 = glm::cross(a - c, b - c);
    double l0 = glm::dot(n0, n0);
    double l1 = glm::dot(n1, n1);
    double l2 = glm::dot(n2, n2);
    glm::dvec3 best = n0;
    if (l1 > l0)
    {
        best = n1;
        l0 = l1;
    }
    if (l2 > l0)
        best = n2;
    const double len = glm::length(best);
    if (!(len > 1e-30))
        return glm::dvec3(0.0, 0.0, 1.0);
    return best / len;
}

/// Newell normal for a closed 3D polygon (outer loop vertex ring); stable on nearly planar n-gons.
static glm::dvec3 NewellUnitNormal(const std::vector<glm::dvec3> &v)
{
    if (v.size() < 3)
        return glm::dvec3(0.0, 0.0, 1.0);
    glm::dvec3 n(0.0);
    const std::size_t nV = v.size();
    for (std::size_t i = 0; i < nV; ++i)
    {
        const glm::dvec3 &vi = v[i];
        const glm::dvec3 &vj = v[(i + 1) % nV];
        n.x += (vi.y - vj.y) * (vi.z + vj.z);
        n.y += (vi.z - vj.z) * (vi.x + vj.x);
        n.z += (vi.x - vj.x) * (vi.y + vj.y);
    }
    const double len = glm::length(n);
    if (!(len > 1e-30))
        return glm::dvec3(0.0, 0.0, 1.0);
    return n / len;
}
} // namespace

void Face::OrientEdgeLoops(const std::vector<std::vector<Edge *>> &edgePtrs)
{
    for (const auto &edgeLoop : edgePtrs)
    {
        std::vector<OrientedEdge> orientedLoop;
        Point *expectedStart = nullptr;

        for (size_t i = 0; i < edgeLoop.size(); i++)
        {
            Edge *edge = edgeLoop[i];
            bool reversed = false;

            if (expectedStart != nullptr)
            {
                if (edge->startPoint == expectedStart)
                {
                    reversed = false;
                }
                else if (edge->endPoint == expectedStart)
                {
                    reversed = true;
                }
                else
                {
                    LOG_WARN("Edge loop is not connected at vertex");
                }
            }
            else if (edgeLoop.size() >= 2)
            {
                // Determine first edge orientation by looking at the second edge
                Edge *next = edgeLoop[1];
                if (edge->endPoint == next->startPoint || edge->endPoint == next->endPoint)
                    reversed = false;
                else if (edge->startPoint == next->startPoint || edge->startPoint == next->endPoint)
                    reversed = true;
            }

            orientedLoop.emplace_back(edge, reversed);
            expectedStart = orientedLoop.back().GetEnd();
        }

        if (!orientedLoop.empty())
        {
            Point *loopStart = orientedLoop[0].GetStart();
            Point *loopEnd = orientedLoop.back().GetEnd();
            if (loopStart != loopEnd)
            {
                LOG_WARN("Edge loop is not closed");
            }
        }

        loops.push_back(orientedLoop);
    }
}

Face::Face(std::vector<std::vector<Edge *>> edgePtrs)
    : surface(std::make_unique<PlanarSurface>()), dependency(nullptr)
{
    OrientEdgeLoops(edgePtrs);
    static_cast<PlanarSurface *>(surface.get())->data = CalculatePlanarData();
}

Face::Face(std::vector<std::vector<Edge *>> edgePtrs, std::unique_ptr<NurbsSurface> nurbs)
    : surface(std::move(nurbs)), dependency(nullptr)
{
    OrientEdgeLoops(edgePtrs);
}

PlanarData Face::CalculatePlanarData()
{
    PlanarData data;
    if (loops.empty() || loops[0].size() < 3)
        return data;

    const auto &outerLoop = loops[0];
    std::vector<glm::dvec3> ring;
    ring.reserve(outerLoop.size());
    for (const OrientedEdge &oe : outerLoop)
    {
        if (oe.edge == nullptr || oe.GetStart() == nullptr)
            return data;
        ring.push_back(oe.GetStartPosition());
    }
    if (ring.size() < 3)
        return data;

    const glm::dvec3 normal =
        ring.size() == 3 ? RobustUnitNormalTriangle(ring[0], ring[1], ring[2]) : NewellUnitNormal(ring);

    data.normal = normal;
    data.d = glm::dot(normal, ring[0]);
    return data;
}
