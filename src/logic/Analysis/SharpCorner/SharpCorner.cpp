#include "SharpCorner.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace
{

double TurnAngle(const glm::dvec3 &prev, const glm::dvec3 &curr, const glm::dvec3 &next)
{
    const glm::dvec2 dirIn = glm::dvec2(curr - prev);
    const glm::dvec2 dirOut = glm::dvec2(next - curr);
    const double lenIn = glm::length(dirIn);
    const double lenOut = glm::length(dirOut);
    if (lenIn < 1e-12 || lenOut < 1e-12)
        return 0.0;
    const double cosTurn = std::clamp(glm::dot(dirIn, dirOut) / (lenIn * lenOut), -1.0, 1.0);
    return std::acos(cosTurn);
}

const Edge *FindSharedEdge(const Face *faceA, const Face *faceB)
{
    for (const auto &loop : faceA->loops)
        for (const auto &oe : loop)
            if (oe.edge && oe.edge->dependencies.count(const_cast<Face *>(faceB)))
                return oe.edge;
    return nullptr;
}

} // namespace

SharpCorner::SharpCorner(double layerHeight, double thresholdDegrees)
    : layerHeight(layerHeight), thresholdRadians(thresholdDegrees * M_PI / 180.0)
{
}

std::vector<FaceFlaw> SharpCorner::Analyze(const Solid *, const std::vector<SlicedLayer> &layers) const
{
    struct EdgeRecord
    {
        double zMin, zMax;
        const Face *faceA, *faceB;
    };
    std::unordered_map<const Edge *, EdgeRecord> edgeMap;

    for (const auto &layer : layers)
    {
        for (const auto &loop : layer.loops)
        {
            const size_t n = loop.ring.size();
            if (n < 3)
                continue;

            for (size_t i = 0; i < n; i++)
            {
                const glm::dvec3 &prev = loop.ring[(i + n - 1) % n];
                const glm::dvec3 &curr = loop.ring[i];
                const glm::dvec3 &next = loop.ring[(i + 1) % n];

                if (TurnAngle(prev, curr, next) < thresholdRadians)
                    continue;

                const Face *faceA = loop.edgeFaces[(i + n - 1) % n];
                const Face *faceB = loop.edgeFaces[i];
                if (!faceA || !faceB)
                    continue;

                const Edge *edge = FindSharedEdge(faceA, faceB);
                if (!edge || !edge->startPoint || !edge->endPoint)
                    continue;

                // Skip horizontal edges — they're layer boundaries and don't cause corner bulging.
                const double edgeDz = std::abs(edge->endPoint->position.z - edge->startPoint->position.z);
                if (edgeDz < layerHeight)
                    continue;

                auto it = edgeMap.find(edge);
                if (it == edgeMap.end())
                    edgeMap.emplace(edge, EdgeRecord{layer.z, layer.z, faceA, faceB});
                else
                {
                    it->second.zMin = std::min(it->second.zMin, layer.z);
                    it->second.zMax = std::max(it->second.zMax, layer.z);
                }
            }
        }
    }

    std::vector<FaceFlaw> results;
    for (const auto &[edge, rec] : edgeMap)
    {
        const double zMin = rec.zMin - layerHeight * 0.5;
        const double zMax = rec.zMax + layerHeight * 0.5;

        glm::dvec3 A = edge->startPoint->position;
        glm::dvec3 B = edge->endPoint->position;
        if (A.z > B.z)
            std::swap(A, B);

        const glm::dvec3 edgeDir = B - A;
        const double dz = edgeDir.z;
        if (dz > 1e-12)
        {
            const double tMin = std::clamp((zMin - A.z) / dz, 0.0, 1.0);
            const double tMax = std::clamp((zMax - A.z) / dz, 0.0, 1.0);
            B = A + tMax * edgeDir;
            A = A + tMin * edgeDir;
        }

        if (rec.faceA && rec.faceA->HasGeometry())
            results.push_back({rec.faceA, FaceFlawKind::SHARP_CORNER, {zMin, zMax}, {A, B}});
    }
    return results;
}
