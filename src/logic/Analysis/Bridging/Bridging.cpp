#include "Bridging.hpp"
#include "logic/Analysis/utils/Slice.hpp"

#include <limits>
#include <cmath>

// Check if a point (in XY) lies inside the polygon formed by segments below
static bool PointInsideContour(const glm::dvec2 &p, const std::vector<Segment> &segments)
{
    // Ray-casting algorithm: count crossings of a horizontal ray from p
    int crossings = 0;
    for (const auto &seg : segments)
    {
        glm::dvec2 a(seg.a.x, seg.a.y);
        glm::dvec2 b(seg.b.x, seg.b.y);

        if ((a.y <= p.y && b.y > p.y) || (b.y <= p.y && a.y > p.y))
        {
            double t = (p.y - a.y) / (b.y - a.y);
            if (p.x < a.x + t * (b.x - a.x))
                crossings++;
        }
    }
    return (crossings % 2) == 1;
}

std::vector<Layer> Bridging::Analyze(const Solid *solid) const
{
    std::vector<Layer> bridgeLayers;

    double zMin = std::numeric_limits<double>::max();
    double zMax = std::numeric_limits<double>::lowest();

    for (const Face *face : solid->faces)
    {
        for (const auto &loop : face->loops)
        {
            for (const auto &orientedEdge : loop)
            {
                const Edge *edge = orientedEdge.edge;
                zMin = std::min(zMin, edge->startPoint->position.z);
                zMax = std::max(zMax, edge->startPoint->position.z);
                zMin = std::min(zMin, edge->endPoint->position.z);
                zMax = std::max(zMax, edge->endPoint->position.z);
            }
        }
    }

    if (zMax - zMin < layerHeight * 2)
        return bridgeLayers;

    auto layers = Slice::Range(solid, zMin, zMax, layerHeight);

    // Compare each layer with the one below it
    for (size_t i = 1; i < layers.size(); i++)
    {
        const auto &current = layers[i].segments;
        const auto &below = layers[i - 1].segments;

        if (current.empty() || below.empty())
            continue;

        std::vector<Segment> bridgeSegments;

        // For each segment in the current layer, check if its midpoint
        // is supported by the layer below
        for (const auto &seg : current)
        {
            glm::dvec3 mid = (seg.a + seg.b) * 0.5;
            double segLen = glm::length(glm::dvec2(seg.b.x - seg.a.x, seg.b.y - seg.a.y));

            if (segLen < minBridgeLength)
                continue;

            // If the midpoint of a current-layer segment is NOT inside
            // the contour of the layer below, it's a bridge
            if (!PointInsideContour(glm::dvec2(mid.x, mid.y), below))
            {
                bridgeSegments.push_back(seg);
            }
        }

        if (!bridgeSegments.empty())
        {
            bridgeLayers.emplace_back(bridgeSegments, Flaw::BRIDGING);
        }
    }

    return bridgeLayers;
}
