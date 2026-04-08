#include "Face.hpp"
#include "utils/log.hpp"

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
    if (!loops.empty() && loops[0].size() >= 3)
    {
        const auto &outerLoop = loops[0];
        const OrientedEdge &oe0 = outerLoop[0];
        const OrientedEdge &oe1 = outerLoop[1];
        const OrientedEdge &oe2 = outerLoop[2];

        const Point *p0 = oe0.GetStart();
        const Point *p1 = oe1.GetStart();
        const Point *p2 = oe2.GetStart();

        if (p0 && p1 && p2)
        {
            glm::dvec3 v1 = p1->position - p0->position;
            glm::dvec3 v2 = p2->position - p0->position;
            glm::dvec3 normal = glm::normalize(glm::cross(v1, v2));

            double d = glm::dot(normal, p0->position);

            data.normal = normal;
            data.d = d;
        }
    }
    return data;
}
