#include "SmallFeature.hpp"
#include "logic/Analysis/utils/Slice.hpp"
#include "Analysis/SharpCorner/SharpCorner.hpp"
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

static glm::dvec2 EdgeXYAtZ(const Edge *edge, double z)
{
    double z0 = edge->startPoint->position.z;
    double z1 = edge->endPoint->position.z;
    double t = (z - z0) / (z1 - z0);
    t = std::clamp(t, 0.0, 1.0);
    glm::dvec3 pos = edge->startPoint->position + t * (edge->endPoint->position - edge->startPoint->position);
    return glm::dvec2(pos);
}

static bool IsInsideSolid(const glm::dvec2 &point, const std::vector<Segment> &segments)
{
    int crossings = 0;
    for (const auto &seg : segments)
    {
        glm::dvec2 a(seg.a);
        glm::dvec2 b(seg.b);
        if ((a.y <= point.y && b.y > point.y) || (b.y <= point.y && a.y > point.y))
        {
            double t = (point.y - a.y) / (b.y - a.y);
            if (point.x < a.x + t * (b.x - a.x))
                crossings++;
        }
    }
    return (crossings % 2) == 1;
}

static bool NearSharpCorner(const Segment &s1, const Segment &s2, double z,
                            const std::vector<EdgeFlaw> &sharpEdges, double radius)
{
    for (const auto &ef : sharpEdges)
    {
        if (z < ef.bounds.zMin || z > ef.bounds.zMax)
            continue;

        glm::dvec2 sp = EdgeXYAtZ(ef.edge, z);

        if (glm::length(glm::dvec2(s1.a) - sp) < radius ||
            glm::length(glm::dvec2(s1.b) - sp) < radius ||
            glm::length(glm::dvec2(s2.a) - sp) < radius ||
            glm::length(glm::dvec2(s2.b) - sp) < radius)
            return true;
    }
    return false;
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

    std::vector<EdgeFlaw> sharpEdges;
    if (sharpCorner)
        sharpEdges = sharpCorner->Analyze(solid);

    // Track hole wall faces (gap is void — just highlight the faces directly)
    struct HoleFaceInfo
    {
        ZBounds zBounds;
        double minWidth;
    };
    std::unordered_map<const Face *, HoleFaceInfo> holeFaces;

    // Track trimmed segments per face for building clip boundaries
    std::unordered_map<const Face *, std::vector<Segment>> faceTrimmedSegments;
    std::unordered_map<const Face *, double> faceMinWidth;
    std::unordered_map<const Face *, bool> faceHasOuterOuterPair;

    // Cached close pairs for second pass
    struct ClosePair
    {
        const Segment *s1;
        const Segment *s2;
        double dist;
        double z;
    };
    std::vector<ClosePair> closePairs;

    auto layers = Slice::Range(solid, zMin, zMax, layerHeight);

    // First pass: identify hole wall faces (gap is void)
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

                if (s1.face && s1.face == s2.face)
                    continue;

                double dist = SegmentToSegmentDist(s1, s2);
                if (dist >= minFeatureSize)
                    continue;

                glm::dvec2 midGap = 0.25 * (glm::dvec2(s1.a) + glm::dvec2(s1.b) +
                                             glm::dvec2(s2.a) + glm::dvec2(s2.b));
                if (!IsInsideSolid(midGap, layer.segments))
                {
                    // Gap is void — record both faces as hole walls
                    for (const Face *face : {s1.face, s2.face})
                    {
                        if (!face)
                            continue;
                        auto it = holeFaces.find(face);
                        if (it == holeFaces.end())
                            holeFaces[face] = {{z, z}, dist};
                        else
                        {
                            it->second.zBounds.zMin = std::min(it->second.zBounds.zMin, z);
                            it->second.zBounds.zMax = std::max(it->second.zBounds.zMax, z);
                            it->second.minWidth = std::min(it->second.minWidth, dist);
                        }
                    }
                }
                else
                {
                    closePairs.push_back({&s1, &s2, dist, z});
                }
            }
        }
    }

    // Second pass: process material-gap pairs, skipping faces adjacent to holes
    for (const auto &cp : closePairs)
    {
        const auto &s1 = *cp.s1;
        const auto &s2 = *cp.s2;
        double dist = cp.dist;
        double z = cp.z;

        if (NearSharpCorner(s1, s2, z, sharpEdges, minFeatureSize))
            continue;

        // If either face is a known hole wall, redirect to hole path
        bool s1IsHole = (s1.face && holeFaces.count(s1.face)) || s1.isHole;
        bool s2IsHole = (s2.face && holeFaces.count(s2.face)) || s2.isHole;
        if (s1IsHole || s2IsHole)
        {
            for (const Face *face : {s1.face, s2.face})
            {
                if (!face)
                    continue;
                bool faceIsHole = holeFaces.count(face) || s1.isHole || s2.isHole;
                if (!faceIsHole)
                    continue;
                auto it = holeFaces.find(face);
                if (it == holeFaces.end())
                    holeFaces[face] = {{z, z}, dist};
                else
                {
                    it->second.zBounds.zMin = std::min(it->second.zBounds.zMin, z);
                    it->second.zBounds.zMax = std::max(it->second.zBounds.zMax, z);
                    it->second.minWidth = std::min(it->second.minWidth, dist);
                }
            }
            continue;
        }

        bool isOuterOuter = !s1.isHole && !s2.isHole;

        // Compute trimmed portions within threshold distance
        auto [t1Min, t1Max] = CloseRange(s1, s2, minFeatureSize);
        auto [t2Min, t2Max] = CloseRange(s2, s1, minFeatureSize);

        bool hasTrim1 = s1.face && t1Min < t1Max;
        bool hasTrim2 = s2.face && t2Min < t2Max;

        glm::dvec3 trim1A, trim1B, trim2A, trim2B;

        if (hasTrim1)
        {
            trim1A = s1.a + t1Min * (s1.b - s1.a);
            trim1B = s1.a + t1Max * (s1.b - s1.a);
            faceTrimmedSegments[s1.face].push_back({trim1A, trim1B, s1.face});
            auto it = faceMinWidth.find(s1.face);
            if (it == faceMinWidth.end())
                faceMinWidth[s1.face] = dist;
            else
                it->second = std::min(it->second, dist);
            if (isOuterOuter)
                faceHasOuterOuterPair[s1.face] = true;
        }
        if (hasTrim2)
        {
            trim2A = s2.a + t2Min * (s2.b - s2.a);
            trim2B = s2.a + t2Max * (s2.b - s2.a);
            faceTrimmedSegments[s2.face].push_back({trim2A, trim2B, s2.face});
            auto it = faceMinWidth.find(s2.face);
            if (it == faceMinWidth.end())
                faceMinWidth[s2.face] = dist;
            else
                it->second = std::min(it->second, dist);
            if (isOuterOuter)
                faceHasOuterOuterPair[s2.face] = true;
        }
    }

    // Emit hole wall faces as simple z-clipped highlights (no clip boundaries)
    for (const auto &[face, info] : holeFaces)
    {
        ZBounds extended = {std::max(info.zBounds.zMin - layerHeight, zMin),
                            std::min(info.zBounds.zMax + layerHeight, zMax)};
        results.push_back({face, Flaw::SMALL_FEATURE, extended});
    }

    // Build clip boundary polygons from trimmed segments
    // Skip faces already identified as hole walls
    for (auto &[face, segments] : faceTrimmedSegments)
    {
        if (holeFaces.count(face))
            continue;

        std::sort(segments.begin(), segments.end(),
                  [](const Segment &a, const Segment &b)
                  { return a.a.z < b.a.z; });

        // Merge segments at same Z level
        std::vector<Segment> merged;
        for (const auto &seg : segments)
        {
            if (merged.empty() || std::abs(seg.a.z - merged.back().a.z) > 1e-10)
            {
                merged.push_back(seg);
            }
            else
            {
                // Same Z: take farthest-apart pair of all 4 endpoints
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
        double height = zb.zMax - zb.zMin;
        double minWidth = faceMinWidth.count(face) ? faceMinWidth[face] : 0.0;

        // Classify: tall narrow outer-outer region = thin section, otherwise small feature
        bool hasOuterOuter = faceHasOuterOuterPair.count(face) && faceHasOuterOuterPair[face];
        bool isThinSection = hasOuterOuter && minWidth > 1e-10 && height / minWidth >= heightToWidthRatio;
        Flaw flawType = isThinSection ? Flaw::THIN_SECTION : Flaw::SMALL_FEATURE;

        if (merged.size() < 2)
        {
            results.push_back({face, flawType, zb});
            continue;
        }

        // Consistent left/right ordering using first segment's direction
        glm::dvec2 refDir = glm::dvec2(merged[0].b - merged[0].a);
        double refLen = glm::length(refDir);
        if (refLen < 1e-10)
        {
            results.push_back({face, flawType, zb});
            continue;
        }
        refDir /= refLen;

        for (auto &seg : merged)
        {
            if (glm::dot(glm::dvec2(seg.b - seg.a), refDir) < 0)
                std::swap(seg.a, seg.b);
        }

        // Build clip boundary: left side up, right side down
        std::vector<glm::dvec3> boundary;
        boundary.reserve(merged.size() * 2);
        for (const auto &seg : merged)
            boundary.push_back(seg.a);
        for (auto it = merged.rbegin(); it != merged.rend(); ++it)
            boundary.push_back(it->b);

        results.push_back({face, flawType, zb, std::move(boundary)});
    }

    return results;
}
