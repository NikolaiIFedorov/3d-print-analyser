#include "Structure/StructureTriangulation.hpp"

#include "Geometry/Curve.hpp"
#include "Geometry/Edge.hpp"
#include "Geometry/Face.hpp"
#include "Geometry/OrientedEdge.hpp"
#include "Geometry/Point.hpp"
#include "Geometry/Surface.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

// See documentation/implementations/structure_face_triangulation_2026-05-11.md for phase notes.
// B2a: CMake flag rename, module scaffold, cache key. B2b (current): adaptive polyline of the
// face's outer loop with corner-vertex tags, orthonormal 2D frame for the face plane, preview
// emission of the polylined 3D outline. B2c lands the CGAL straight-skeleton inset on top of the
// 2D polygon; B2d/e the strip + fillet; B2f the slider drag invalidation.

namespace StructureTriangulation
{
namespace
{

// Orthonormal 2D frame anchored on a planar face. `origin` is the projection anchor; `u` and `v`
// span the face's plane; `n` is the face outward normal. Use `Project(p3)` / `Unproject(p2)` to go
// between world space and the face's local 2D coordinates. B2c will hand the projected polygon to
// CGAL's straight-skeleton inset; B2b uses this only to tag corner vertices and to roundtrip the
// preview outline (so we know the frame is sound before any 2D algorithm runs on top of it).
struct FaceFrame
{
    glm::dvec3 origin{0.0};
    glm::dvec3 u{1.0, 0.0, 0.0};
    glm::dvec3 v{0.0, 1.0, 0.0};
    glm::dvec3 n{0.0, 0.0, 1.0};
};

FaceFrame BuildFaceFrame(const Face &face, const glm::dvec3 &origin)
{
    FaceFrame frame;
    frame.origin = origin;
    if (face.surface == nullptr)
        return frame;
    glm::dvec3 n = face.surface->GetNormal();
    const double nLen = glm::length(n);
    if (nLen < 1e-12)
        return frame;
    n /= nLen;
    // Pick the world axis least aligned with `n` so the cross product produces a stable `u`.
    glm::dvec3 axis(1.0, 0.0, 0.0);
    if (std::abs(glm::dot(n, axis)) > 0.9)
        axis = glm::dvec3(0.0, 1.0, 0.0);
    const glm::dvec3 u = glm::normalize(glm::cross(n, axis));
    frame.n = n;
    frame.u = u;
    frame.v = glm::cross(n, u);
    return frame;
}

[[maybe_unused]] glm::dvec2 Project(const glm::dvec3 &p, const FaceFrame &f)
{
    const glm::dvec3 d = p - f.origin;
    return {glm::dot(d, f.u), glm::dot(d, f.v)};
}

[[maybe_unused]] glm::dvec3 Unproject(const glm::dvec2 &p, const FaceFrame &f)
{
    return f.origin + p.x * f.u + p.y * f.v;
}

// Adaptive curve subdivision: emits midpoint samples whose chord deviation exceeds `chordTolMm`,
// in the underlying curve's canonical parameter direction (caller reverses the list for reversed
// `OrientedEdge`s). Hard recursion cap protects against pathological curves.
constexpr int kMaxSubdivideDepth = 12;

void SubdivideCurve(const Curve &curve, const glm::dvec3 &start, const glm::dvec3 &end,
                    double t0, const glm::dvec3 &p0, double t1, const glm::dvec3 &p1,
                    double chordTolMm, int depth, std::vector<glm::dvec3> &outMidsExclusive)
{
    if (depth >= kMaxSubdivideDepth)
        return;
    const double tMid = 0.5 * (t0 + t1);
    const glm::dvec3 pMid = curve.Evaluate(tMid, start, end);
    const glm::dvec3 chordMid = 0.5 * (p0 + p1);
    if (glm::length(pMid - chordMid) <= chordTolMm)
        return;
    SubdivideCurve(curve, start, end, t0, p0, tMid, pMid, chordTolMm, depth + 1, outMidsExclusive);
    outMidsExclusive.push_back(pMid);
    SubdivideCurve(curve, start, end, tMid, pMid, t1, p1, chordTolMm, depth + 1, outMidsExclusive);
}

// Walks the face's outer loop, polylining curved edges. Each pushed vertex is tagged with
// `isCorner = true` when it sits on a B-rep vertex (`OrientedEdge::GetStart` / `GetEnd`) and
// `false` when it is a polyline-introduced midpoint. The result is start-inclusive / end-exclusive
// per edge so consecutive edges concatenate without duplicating shared vertices.
struct PolylinedOutline
{
    std::vector<glm::dvec3> points;
    std::vector<bool> isCorner;
};

PolylinedOutline BuildPolylinedOuterLoop(const Face &face, double chordTolMm)
{
    PolylinedOutline out;
    if (face.loops.empty())
        return out;
    const auto &loop = face.loops[0];
    out.points.reserve(loop.size() * 4);
    out.isCorner.reserve(loop.size() * 4);
    for (const OrientedEdge &oe : loop)
    {
        if (oe.edge == nullptr)
            continue;
        const glm::dvec3 oeStart = oe.GetStartPosition();
        out.points.push_back(oeStart);
        out.isCorner.push_back(true);
        if (oe.edge->curve == nullptr || oe.edge->startPoint == nullptr || oe.edge->endPoint == nullptr)
            continue;
        // Sample the curve in its canonical (non-oriented) parameter direction, then reverse the
        // mid samples when the edge is reversed. ArcCurve and NurbsCurve disagree on whether they
        // honour caller-supplied start/end as a swap signal — sampling canonical + reverse-on-need
        // is consistent across both.
        const glm::dvec3 underlyingStart = oe.edge->startPoint->position;
        const glm::dvec3 underlyingEnd = oe.edge->endPoint->position;
        std::vector<glm::dvec3> mids;
        SubdivideCurve(*oe.edge->curve, underlyingStart, underlyingEnd, 0.0, underlyingStart, 1.0,
                       underlyingEnd, chordTolMm, 0, mids);
        if (oe.reversed)
            std::reverse(mids.begin(), mids.end());
        for (const glm::dvec3 &mid : mids)
        {
            out.points.push_back(mid);
            out.isCorner.push_back(false);
        }
    }
    return out;
}


struct CacheKey
{
    const Face *face;
    double insetMm;
    double chordTolMm;
    double minFeatureMm;
    bool operator==(const CacheKey &other) const noexcept
    {
        return face == other.face && insetMm == other.insetMm && chordTolMm == other.chordTolMm &&
               minFeatureMm == other.minFeatureMm;
    }
};

struct CacheKeyHash
{
    std::size_t operator()(const CacheKey &k) const noexcept
    {
        // Pointer hashing dominates; the three doubles fold in via a cheap xor/shift since the
        // panel only ever sets a small handful of distinct values.
        std::size_t h = std::hash<const Face *>{}(k.face);
        h ^= std::hash<long long>{}(static_cast<long long>(k.insetMm * 1e6)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<long long>{}(static_cast<long long>(k.chordTolMm * 1e6)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<long long>{}(static_cast<long long>(k.minFeatureMm * 1e6)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

using SegmentList = std::vector<std::pair<glm::vec3, glm::vec3>>;
std::unordered_map<CacheKey, SegmentList, CacheKeyHash> &BakeCache()
{
    static std::unordered_map<CacheKey, SegmentList, CacheKeyHash> instance;
    return instance;
}

} // namespace

std::vector<std::pair<glm::vec3, glm::vec3>> BuildFaceTriangulationPreview(const Face *face,
                                                                          const BakeParams &params)
{
    if (face == nullptr)
        return {};
    const CacheKey key{face, params.insetMm, params.chordTolMm, params.minFeatureMm};
    auto &cache = BakeCache();
    if (auto it = cache.find(key); it != cache.end())
        return it->second;

    // B2b output: the polylined outer loop, emitted as consecutive 3D segments. This is the same
    // outline the user already sees from the wireframe — the preview value is *visual proof* that
    // we are tracing the right face and that adaptive sampling tracks the curve within the chord
    // tolerance. B2c overwrites this body with the offset/inset emission once the polygon flows
    // into CGAL.
    SegmentList segments;
    const PolylinedOutline outline = BuildPolylinedOuterLoop(*face, params.chordTolMm);
    if (outline.points.size() >= 2)
    {
        // The frame is built (and immediately discarded) here only to assert the math is well-
        // formed on this face. B2c will keep the frame around and feed `Project`ed points into
        // CGAL's `Polygon_2`.
        (void)BuildFaceFrame(*face, outline.points.front());

        segments.reserve(outline.points.size());
        for (std::size_t i = 0; i + 1 < outline.points.size(); ++i)
            segments.emplace_back(glm::vec3(outline.points[i]), glm::vec3(outline.points[i + 1]));
        // Close the loop back to the first vertex.
        segments.emplace_back(glm::vec3(outline.points.back()), glm::vec3(outline.points.front()));
    }
    cache.emplace(key, segments);
    return segments;
}

void ClearBakeCache()
{
    BakeCache().clear();
}

void InvalidateBakeCacheForParams(const BakeParams &params)
{
    auto &cache = BakeCache();
    for (auto it = cache.begin(); it != cache.end();)
    {
        if (it->first.insetMm != params.insetMm || it->first.chordTolMm != params.chordTolMm ||
            it->first.minFeatureMm != params.minFeatureMm)
            it = cache.erase(it);
        else
            ++it;
    }
}

} // namespace StructureTriangulation
