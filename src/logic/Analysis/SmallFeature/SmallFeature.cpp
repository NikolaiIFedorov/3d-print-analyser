#include "SmallFeature.hpp"
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

static std::pair<double, double> CloseRange(const Segment &s1, const Segment &s2,
                                            double threshold, int samples = 20)
{
    double tMin = 1.0, tMax = 0.0;
    for (int i = 0; i <= samples; i++)
    {
        double t = static_cast<double>(i) / samples;
        glm::dvec3 p = s1.a + t * (s1.b - s1.a);
        glm::dvec3 closest = ClosestPointOnSegment(p, s2.a, s2.b);
        double dist = glm::length(glm::dvec2(p) - glm::dvec2(closest));
        if (dist < threshold)
        {
            tMin = std::min(tMin, t);
            tMax = std::max(tMax, t);
        }
    }
    return {tMin, tMax};
}

std::vector<FaceFlaw> SmallFeature::Analyze(const Solid *solid, std::optional<ZBounds> bounds,
                                            std::vector<BridgeSurface> *bridgeSurfaces) const
{
    std::vector<FaceFlaw> results;

    auto [zMin, zMax] = bounds.value_or(Slice::GetZBounds(solid));

    if (zMax - zMin < layerHeight)
        return results;

    std::unordered_map<const Face *, std::vector<Segment>> faceTrimmedSegments;
    std::unordered_map<const Face *, double> faceMinWidth;

    auto layers = Slice::Range(solid, zMin, zMax, layerHeight);

    for (const auto &layer : layers)
    {
        if (layer.segments.size() < 2)
            continue;

        for (size_t i = 0; i < layer.segments.size(); i++)
        {
            const auto &s1 = layer.segments[i];

            for (size_t j = i + 1; j < layer.segments.size(); j++)
            {
                const auto &s2 = layer.segments[j];

                if (SharesEndpoint(s1, s2))
                    continue;

                if (s1.face && s1.face == s2.face)
                    continue;

                double dist = SegmentToSegmentDist(s1, s2);
                if (dist >= minFeatureSize)
                    continue;

                auto [t1Min, t1Max] = CloseRange(s1, s2, minFeatureSize);
                auto [t2Min, t2Max] = CloseRange(s2, s1, minFeatureSize);

                if (s1.face && t1Min < t1Max)
                {
                    glm::dvec3 trim1A = s1.a + t1Min * (s1.b - s1.a);
                    glm::dvec3 trim1B = s1.a + t1Max * (s1.b - s1.a);
                    faceTrimmedSegments[s1.face].push_back({trim1A, trim1B, s1.face});
                    auto it = faceMinWidth.find(s1.face);
                    if (it == faceMinWidth.end())
                        faceMinWidth[s1.face] = dist;
                    else
                        it->second = std::min(it->second, dist);
                }

                if (s2.face && t2Min < t2Max)
                {
                    glm::dvec3 trim2A = s2.a + t2Min * (s2.b - s2.a);
                    glm::dvec3 trim2B = s2.a + t2Max * (s2.b - s2.a);
                    faceTrimmedSegments[s2.face].push_back({trim2A, trim2B, s2.face});
                    auto it = faceMinWidth.find(s2.face);
                    if (it == faceMinWidth.end())
                        faceMinWidth[s2.face] = dist;
                    else
                        it->second = std::min(it->second, dist);
                }
            }
        }
    }

    // Build clip boundary polygons from trimmed segments per face
    for (auto &[face, segments] : faceTrimmedSegments)
    {
        std::sort(segments.begin(), segments.end(),
                  [](const Segment &a, const Segment &b)
                  { return a.a.z < b.a.z; });

        // Merge segments at the same Z level into a single spanning segment
        std::vector<Segment> merged;
        for (const auto &seg : segments)
        {
            if (merged.empty() || std::abs(seg.a.z - merged.back().a.z) > 1e-10)
            {
                merged.push_back(seg);
            }
            else
            {
                // Same Z: take the farthest-apart pair of all 4 endpoints
                auto &last = merged.back();
                glm::dvec3 pts[4] = {last.a, last.b, seg.a, seg.b};
                double maxDist = 0;
                int mi = 0, mj = 1;
                for (int p = 0; p < 4; p++)
                    for (int q = p + 1; q < 4; q++)
                    {
                        double d = glm::length(glm::dvec2(pts[p] - pts[q]));
                        if (d > maxDist)
                        {
                            maxDist = d;
                            mi = p;
                            mj = q;
                        }
                    }
                last.a = pts[mi];
                last.b = pts[mj];
            }
        }

        ZBounds zb = {merged.front().a.z, merged.back().a.z};

        if (merged.size() < 2)
        {
            results.push_back({face, FaceFlawKind::SMALL_FEATURE, zb});
            continue;
        }

        // Consistent left/right ordering using the first segment's direction
        glm::dvec2 refDir = glm::dvec2(merged[0].b - merged[0].a);
        double refLen = glm::length(refDir);
        if (refLen < 1e-10)
        {
            results.push_back({face, FaceFlawKind::SMALL_FEATURE, zb});
            continue;
        }
        refDir /= refLen;

        for (auto &seg : merged)
        {
            if (glm::dot(glm::dvec2(seg.b - seg.a), refDir) < 0)
                std::swap(seg.a, seg.b);
        }

        // Build clip boundary: left side ascending, right side descending
        std::vector<glm::dvec3> boundary;
        boundary.reserve(merged.size() * 2);
        for (const auto &seg : merged)
            boundary.push_back(seg.a);
        for (auto it = merged.rbegin(); it != merged.rend(); ++it)
            boundary.push_back(it->b);

        results.push_back({face, FaceFlawKind::SMALL_FEATURE, zb, std::move(boundary)});
    }

    return results;
}
