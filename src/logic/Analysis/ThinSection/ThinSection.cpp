#include "ThinSection.hpp"
#include "logic/Analysis/utils/Slice.hpp"
#include "scene/Geometry/AllGeometry.hpp"

static glm::dvec3 ClosestPointOnSegment(const glm::dvec3 &p, const glm::dvec3 &a, const glm::dvec3 &b)
{
    glm::dvec2 ab = glm::dvec2(b) - glm::dvec2(a);
    glm::dvec2 ap = glm::dvec2(p) - glm::dvec2(a);
    double t = glm::dot(ap, ab) / glm::dot(ab, ab);
    t = std::clamp(t, 0.0, 1.0);
    return a + t * (b - a);
}

static bool SharesEndpoint(const Segment &s1, const Segment &s2)
{
    const double eps = 1e-10;
    return glm::length(glm::dvec2(s1.a - s2.a)) < eps ||
           glm::length(glm::dvec2(s1.a - s2.b)) < eps ||
           glm::length(glm::dvec2(s1.b - s2.a)) < eps ||
           glm::length(glm::dvec2(s1.b - s2.b)) < eps;
}

static double SegmentToSegmentDist(const Segment &s1, const Segment &s2)
{
    double d1 = glm::length(glm::dvec2(ClosestPointOnSegment(s1.a, s2.a, s2.b)) - glm::dvec2(s1.a));
    double d2 = glm::length(glm::dvec2(ClosestPointOnSegment(s1.b, s2.a, s2.b)) - glm::dvec2(s1.b));
    double d3 = glm::length(glm::dvec2(ClosestPointOnSegment(s2.a, s1.a, s1.b)) - glm::dvec2(s2.a));
    double d4 = glm::length(glm::dvec2(ClosestPointOnSegment(s2.b, s1.a, s1.b)) - glm::dvec2(s2.b));
    return std::min({d1, d2, d3, d4});
}

std::vector<FaceFlaw> ThinSection::Analyze(const Solid *solid, std::optional<ZBounds> bounds,
                                           std::vector<BridgeSurface> *bridgeSurfaces) const
{
    std::vector<FaceFlaw> results;

    auto [zMin, zMax] = bounds.value_or(Slice::GetZBounds(solid));

    if (zMax - zMin < layerHeight)
        return results;

    // Per face: track Z range and narrowest width observed
    struct FaceInfo
    {
        ZBounds zBounds;
        double minWidth;
    };
    std::unordered_map<const Face *, FaceInfo> faceInfos;

    auto layers = Slice::Range(solid, zMin, zMax, layerHeight);

    for (const auto &layer : layers)
    {
        if (layer.segments.size() < 2)
            continue;

        double z = layer.segments[0].a.z;

        for (size_t i = 0; i < layer.segments.size(); i++)
        {
            const auto &s1 = layer.segments[i];

            for (size_t j = i + 1; j < layer.segments.size(); j++)
            {
                const auto &s2 = layer.segments[j];

                if (SharesEndpoint(s1, s2))
                    continue;

                double dist = SegmentToSegmentDist(s1, s2);
                if (dist >= widthThreshold)
                    continue;

                // Record both faces with their Z level and observed width
                for (const Face *face : {s1.face, s2.face})
                {
                    if (!face)
                        continue;
                    auto it = faceInfos.find(face);
                    if (it == faceInfos.end())
                        faceInfos[face] = {{z, z}, dist};
                    else
                    {
                        it->second.zBounds.zMin = std::min(it->second.zBounds.zMin, z);
                        it->second.zBounds.zMax = std::max(it->second.zBounds.zMax, z);
                        it->second.minWidth = std::min(it->second.minWidth, dist);
                    }
                }
            }
        }
    }

    // Only flag faces where the narrow region spans enough height relative to width
    for (const auto &[face, info] : faceInfos)
    {
        double height = info.zBounds.zMax - info.zBounds.zMin;
        if (info.minWidth < 1e-10)
            continue;
        if (height / info.minWidth >= heightToWidthRatio)
            results.push_back({face, Flaw::THIN_SECTION, info.zBounds});
    }

    return results;
}