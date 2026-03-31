#include "SharpCorner.hpp"

#include <unordered_set>

std::vector<Layer> SharpCorner::Analyze(const Solid *solid) const
{
    std::vector<Layer> layers;

    // Collect all unique edges in the solid
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

                // An edge needs exactly 2 adjacent faces to compute a dihedral angle
                if (edge->dependencies.size() != 2)
                    continue;

                auto it = edge->dependencies.begin();
                const Face *f1 = *it++;
                const Face *f2 = *it;

                // Both faces must be planar for a meaningful dihedral angle
                if (!f1->GetSurface().IsPlanar() || !f2->GetSurface().IsPlanar())
                    continue;

                glm::dvec3 n1 = glm::normalize(f1->GetSurface().GetNormal());
                glm::dvec3 n2 = glm::normalize(f2->GetSurface().GetNormal());

                // cos(dihedral) — a value close to 1 means faces are nearly coplanar,
                // a value close to -1 means a very sharp internal corner
                double cosAngle = glm::dot(n1, n2);

                if (cosAngle < cosThreshold)
                {
                    Segment seg{edge->startPoint->position, edge->endPoint->position};
                    layers.emplace_back(std::vector<Segment>{seg}, Flaw::SHARP_CORNER);
                }
            }
        }
    }

    return layers;
}
