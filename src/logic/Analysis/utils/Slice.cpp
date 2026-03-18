#include "Slice.hpp"
#include "scene/Geometry/AllGeometry.hpp"
#include <limits>

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

// Check if two segments share an endpoint (adjacent in the contour)
static bool SharesEndpoint(const Segment &s1, const Segment &s2)
{
    const double eps = 1e-10;
    return glm::length(glm::dvec2(s1.a - s2.a)) < eps ||
           glm::length(glm::dvec2(s1.a - s2.b)) < eps ||
           glm::length(glm::dvec2(s1.b - s2.a)) < eps ||
           glm::length(glm::dvec2(s1.b - s2.b)) < eps;
}

std::vector<Segment> Slice::At(const Solid *solid, double z)
{
    std::vector<Segment> segments;

    for (const Face *face : solid->faces)
    {
        for (const auto &loop : face->loops)
        {
            std::vector<glm::dvec3> intersections;
            for (const auto &orientedEdge : loop)
            {
                const Edge *edge = orientedEdge.edge;
                glm::dvec3 p0 = edge->startPoint->position;
                glm::dvec3 p1 = edge->endPoint->position;

                double zMin = std::min(p0.z, p1.z);
                double zMax = std::max(p0.z, p1.z);

                if (z < zMin || z > zMax)
                    continue;

                double dz = p1.z - p0.z;
                if (std::abs(dz) < 1e-10)
                    continue;

                double t = (z - p0.z) / dz;
                double x = p0.x + t * (p1.x - p0.x);
                double y = p0.y + t * (p1.y - p0.y);

                intersections.push_back({x, y, z});
            }

            for (size_t i = 0; i + 1 < intersections.size(); i += 2)
            {
                segments.push_back({intersections[i], intersections[i + 1]});
            }
        }
    }

    return segments;
}

std::vector<Layer> Slice::Range(const Solid *solid, double zMin, double zMax, double layerHeight)
{
    std::vector<Layer> layers;

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
