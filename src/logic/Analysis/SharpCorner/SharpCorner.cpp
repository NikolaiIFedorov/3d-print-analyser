#include "SharpCorner.hpp"
#include "utils/log.hpp"
#include <unordered_set>
#include <algorithm>

std::vector<EdgeFlaw> SharpCorner::Analyze(const Solid *solid) const
{
    std::vector<EdgeFlaw> results;

    std::unordered_set<Edge *> visited;

    for (const Face *face : solid->faces)
    {
        for (const auto &loop : face->loops)
        {
            for (const auto &orientedEdge : loop)
            {
                Edge *edge = orientedEdge.edge;
                if (!visited.insert(edge).second)
                    continue;

                if (edge->dependencies.size() != 2)
                    continue;

                auto it = edge->dependencies.begin();
                const Face *f1 = *it++;
                const Face *f2 = *it;

                if (!f1->GetSurface().IsPlanar() || !f2->GetSurface().IsPlanar())
                    continue;

                glm::dvec3 n1 = glm::normalize(f1->GetSurface().GetNormal());
                glm::dvec3 n2 = glm::normalize(f2->GetSurface().GetNormal());

                double cosAngle = glm::dot(n1, n2);

                if (cosAngle < cosThreshold)
                {
                    double zStart = edge->startPoint->position.z;
                    double zEnd = edge->endPoint->position.z;
                    double zMin = std::min(zStart, zEnd);
                    double zMax = std::max(zStart, zEnd);

                    results.push_back({edge, Flaw::SHARP_CORNER, {zMin, zMax}});
                }
            }
        }
    }
    LOG_DEBU(Log::NumToStr(results.size()) + " sharp corner edge flaws found");
    return results;
}
