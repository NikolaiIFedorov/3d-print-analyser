#include "LayerDiffUtils.hpp"
#include "scene/Geometry/Face.hpp"
#include "GeometryOps/ConvexHull2D.hpp"
#include "GeometryOps/RingUtils.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace LayerDiffUtils
{

std::vector<GeometryOps::ClassifiedRing> ToClassifiedRings(const std::vector<SliceLoop> &loops)
{
    std::vector<GeometryOps::ClassifiedRing> rings;
    rings.reserve(loops.size());
    for (const auto &loop : loops)
        rings.push_back({loop.ring, loop.isHole});
    return rings;
}

namespace
{
double PointToSegmentDistXY(const glm::dvec3 &p, const glm::dvec3 &a, const glm::dvec3 &b)
{
    const glm::dvec2 ab = glm::dvec2(b) - glm::dvec2(a);
    const glm::dvec2 ap = glm::dvec2(p) - glm::dvec2(a);
    const double denom = glm::dot(ab, ab);
    double t = denom > 1e-20 ? glm::dot(ap, ab) / denom : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const glm::dvec2 closest = glm::dvec2(a) + t * ab;
    return glm::length(glm::dvec2(p) - closest);
}
} // namespace

const Face *NearestEdgeFace(const glm::dvec3 &p, const std::vector<SliceLoop> &loops)
{
    const Face *best = nullptr;
    double bestDist = std::numeric_limits<double>::max();
    for (const auto &loop : loops)
    {
        const size_t n = loop.ring.size();
        for (size_t i = 0; i < n; i++)
        {
            const glm::dvec3 &a = loop.ring[i];
            const glm::dvec3 &b = loop.ring[(i + 1) % n];
            const double dist = PointToSegmentDistXY(p, a, b);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = (i < loop.edgeFaces.size()) ? loop.edgeFaces[i] : nullptr;
            }
        }
    }
    return best;
}

std::unordered_set<const Face *> FacesForDiffRing(const std::vector<glm::dvec3> &diffRing,
                                                   const std::vector<SliceLoop> &loops,
                                                   double tolerance,
                                                   double overhangAngleDeg)
{
    // A face qualifies only if its normal dips below this Z threshold, meaning it is itself
    // overhanging at or beyond the angle — vertical walls and upward faces are excluded.
    const double minNormalZ = -std::cos(glm::radians(overhangAngleDeg));

    std::unordered_map<const Face *, size_t> counts;
    const double tol2 = tolerance * tolerance;
    const size_t n = diffRing.size();

    for (size_t e = 0; e < n; e++)
    {
        const glm::dvec2 da = glm::dvec2(diffRing[e]);
        const glm::dvec2 db = glm::dvec2(diffRing[(e + 1) % n]);

        const Face *matched = nullptr;
        for (const auto &loop : loops)
        {
            const size_t m = loop.ring.size();
            for (size_t i = 0; i < m; i++)
            {
                const glm::dvec2 la = glm::dvec2(loop.ring[i]);
                const glm::dvec2 lb = glm::dvec2(loop.ring[(i + 1) % m]);
                auto dist2 = [](const glm::dvec2 &p, const glm::dvec2 &q) { glm::dvec2 d = p - q; return glm::dot(d, d); };
                bool forward = dist2(da, la) < tol2 && dist2(db, lb) < tol2;
                bool reverse = dist2(da, lb) < tol2 && dist2(db, la) < tol2;
                if (forward || reverse)
                {
                    matched = (i < loop.edgeFaces.size()) ? loop.edgeFaces[i] : nullptr;
                    break;
                }
            }
            if (matched)
                break;
        }

        if (!matched)
        {
            const glm::dvec3 mid = 0.5 * (diffRing[e] + diffRing[(e + 1) % n]);
            matched = NearestEdgeFace(mid, loops);
        }

        if (matched && matched->HasGeometry() &&
            matched->GetSurface().GetNormal().z < minNormalZ)
            counts[matched]++;
    }

    // Return the dominant eligible face (most attributed diff ring edges).
    const Face *best = nullptr;
    size_t bestCount = 0;
    for (const auto &[face, count] : counts)
    {
        if (count > bestCount)
        {
            bestCount = count;
            best = face;
        }
    }

    std::unordered_set<const Face *> result;
    if (best)
        result.insert(best);
    return result;
}

bool LayersAreEffectivelyIdentical(const std::vector<SliceLoop> &a, const std::vector<SliceLoop> &b,
                                   double areaEpsMm2, double centroidEpsMm)
{
    if (a.size() != b.size())
        return false;

    std::vector<bool> matched(b.size(), false);
    for (const auto &loopA : a)
    {
        if (loopA.ring.size() < 3)
            return false; // degenerate — don't trust the cheap path, fall through to the exact one

        const glm::dvec2 centroidA = GeometryOps::RingCentroidXy(loopA.ring);
        const double areaA = std::abs(GeometryOps::SignedArea2D(loopA.ring, GeometryOps::kWorldUpNormal));

        int bestIdx = -1;
        double bestDist = std::numeric_limits<double>::max();
        for (size_t j = 0; j < b.size(); j++)
        {
            if (matched[j])
                continue;
            const double d = glm::length(centroidA - GeometryOps::RingCentroidXy(b[j].ring));
            if (d < bestDist)
            {
                bestDist = d;
                bestIdx = static_cast<int>(j);
            }
        }

        if (bestIdx < 0)
            return false;

        const double areaB = std::abs(GeometryOps::SignedArea2D(b[bestIdx].ring, GeometryOps::kWorldUpNormal));
        if (bestDist > centroidEpsMm || std::abs(areaA - areaB) > areaEpsMm2 || loopA.isHole != b[bestIdx].isHole)
            return false;

        matched[bestIdx] = true;
    }

    return true;
}

bool AnyLoopIsDegenerate(const std::vector<SliceLoop> &loops, double minWidthEpsMm)
{
    for (const auto &loop : loops)
    {
        if (loop.ring.size() < 3)
            return true;
        if (GeometryOps::MinWidth2D(loop.ring, GeometryOps::kWorldUpNormal) < minWidthEpsMm)
            return true;
    }
    return false;
}

} // namespace LayerDiffUtils
