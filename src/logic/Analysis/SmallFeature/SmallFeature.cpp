#include "SmallFeature.hpp"
#include "logic/Analysis/utils/Slice.hpp"
#include "Analysis/SharpCorner/SharpCorner.hpp"
#include "scene/Geometry/AllGeometry.hpp"
#include <map>

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

    // Track trimmed segments per face for building clip boundaries
    std::unordered_map<const Face *, std::vector<Segment>> faceTrimmedSegments;
    std::unordered_map<const Face *, double> faceMinWidth;
    std::unordered_map<const Face *, bool> faceHasOuterOuterPair;

    // Track bridge data per face pair: pairs of trim segments at each Z level
    using FacePairKey = std::pair<const Face *, const Face *>;
    struct BridgeLayerData
    {
        double z;
        glm::dvec3 a1, b1; // trim endpoints on face 1
        glm::dvec3 a2, b2; // trim endpoints on face 2
    };
    std::map<FacePairKey, std::vector<BridgeLayerData>> facePairBridges;

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
                if (dist >= minFeatureSize)
                    continue;

                if (NearSharpCorner(s1, s2, z, sharpEdges, minFeatureSize))
                    continue;

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

                // Record bridge pair connecting the two faces (grouped by face pair)
                if (bridgeSurfaces && hasTrim1 && hasTrim2)
                {
                    const Face *f1 = s1.face;
                    const Face *f2 = s2.face;
                    FacePairKey key = (f1 < f2) ? FacePairKey{f1, f2} : FacePairKey{f2, f1};

                    BridgeLayerData bld;
                    bld.z = z;
                    if (f1 < f2)
                    {
                        bld.a1 = trim1A;
                        bld.b1 = trim1B;
                        bld.a2 = trim2A;
                        bld.b2 = trim2B;
                    }
                    else
                    {
                        bld.a1 = trim2A;
                        bld.b1 = trim2B;
                        bld.a2 = trim1A;
                        bld.b2 = trim1B;
                    }
                    facePairBridges[key].push_back(bld);
                }
            }
        }
    }

    // Build clip boundary polygons from trimmed segments
    for (auto &[face, segments] : faceTrimmedSegments)
    {
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

    // Build vertical connecting surfaces from face-pair bridge data
    if (bridgeSurfaces)
    {
        for (auto &[key, layers] : facePairBridges)
        {
            if (layers.size() < 2)
                continue;

            std::sort(layers.begin(), layers.end(),
                      [](const BridgeLayerData &a, const BridgeLayerData &b)
                      { return a.z < b.z; });

            // Determine a consistent direction for a/b endpoints using the first layer
            glm::dvec2 refDir1 = glm::dvec2(layers[0].b1 - layers[0].a1);
            double refLen1 = glm::length(refDir1);
            glm::dvec2 refDir2 = glm::dvec2(layers[0].b2 - layers[0].a2);
            double refLen2 = glm::length(refDir2);

            if (refLen1 > 1e-10)
            {
                refDir1 /= refLen1;
                for (auto &ld : layers)
                    if (glm::dot(glm::dvec2(ld.b1 - ld.a1), refDir1) < 0)
                        std::swap(ld.a1, ld.b1);
            }
            if (refLen2 > 1e-10)
            {
                refDir2 /= refLen2;
                for (auto &ld : layers)
                    if (glm::dot(glm::dvec2(ld.b2 - ld.a2), refDir2) < 0)
                        std::swap(ld.a2, ld.b2);
            }

            // Left side: a1 going up, a2 going down
            std::vector<glm::dvec3> leftBoundary;
            for (const auto &ld : layers)
                leftBoundary.push_back(ld.a1);
            for (auto it = layers.rbegin(); it != layers.rend(); ++it)
                leftBoundary.push_back(it->a2);

            // Right side: b1 going up, b2 going down
            std::vector<glm::dvec3> rightBoundary;
            for (const auto &ld : layers)
                rightBoundary.push_back(ld.b1);
            for (auto it = layers.rbegin(); it != layers.rend(); ++it)
                rightBoundary.push_back(it->b2);

            // Inherit flaw type from connected faces
            Flaw flaw = Flaw::SMALL_FEATURE;
            for (const auto &r : results)
            {
                if (r.face == key.first || r.face == key.second)
                {
                    flaw = r.flaw;
                    break;
                }
            }
            if (leftBoundary.size() >= 3)
                bridgeSurfaces->push_back({flaw, std::move(leftBoundary)});
            if (rightBoundary.size() >= 3)
                bridgeSurfaces->push_back({flaw, std::move(rightBoundary)});
        }
    }

    return results;
}
