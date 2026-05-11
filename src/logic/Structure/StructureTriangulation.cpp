#include "Structure/StructureTriangulation.hpp"

#include "Geometry/Curve.hpp"
#include "Geometry/Edge.hpp"
#include "Geometry/Face.hpp"
#include "Geometry/OrientedEdge.hpp"
#include "Geometry/Point.hpp"
#include "Geometry/Surface.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <unordered_map>

#if defined(CAD_USE_CGAL)
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Polygon_2.h>
#include <CGAL/create_offset_polygons_2.h>
#endif

// See documentation/implementations/structure_face_triangulation_2026-05-11.md for phase notes.
// B2a: CMake flag rename, module scaffold, cache key. B2b (current): adaptive polyline of the
// face's outer loop with corner-vertex tags, orthonormal 2D frame for the face plane, preview
// emission of the polylined 3D outline. B2c lands the CGAL straight-skeleton inset on top of the
// 2D polygon; B2d/e the strip + fillet; B2f the slider drag invalidation.

namespace StructureTriangulation
{
namespace
{

using SegmentList = std::vector<std::pair<glm::vec3, glm::vec3>>;

#if defined(CAD_USE_CGAL)
using SkeletonKernel = CGAL::Exact_predicates_inexact_constructions_kernel;
using SkeletonPoint = SkeletonKernel::Point_2;
using SkeletonPolygon = CGAL::Polygon_2<SkeletonKernel>;
#endif

// Orthonormal 2D frame anchored on a planar face. `origin` is the projection anchor; `u` and `v`
// span the face's plane; `n` is the face outward normal. Use `Project(p3)` / `Unproject(p2)` to go
// between world space and the face's local 2D coordinates. CGAL's straight-skeleton (B2c) reads the
// `Polygon_2` built from `Project`ed points; `Unproject` lifts the offset back to 3D for preview.
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

glm::dvec2 Project(const glm::dvec3 &p, const FaceFrame &f)
{
    const glm::dvec3 d = p - f.origin;
    return {glm::dot(d, f.u), glm::dot(d, f.v)};
}

glm::dvec3 Unproject(const glm::dvec2 &p, const FaceFrame &f)
{
    return f.origin + p.x * f.u + p.y * f.v;
}

void AppendRingAsSegments(const std::vector<glm::dvec3> &ring, SegmentList &out)
{
    if (ring.size() < 2)
        return;
    out.reserve(out.size() + ring.size());
    for (std::size_t i = 0; i + 1 < ring.size(); ++i)
        out.emplace_back(glm::vec3(ring[i]), glm::vec3(ring[i + 1]));
    out.emplace_back(glm::vec3(ring.back()), glm::vec3(ring.front()));
}

#if defined(CAD_USE_CGAL)

// Builds a CGAL `Polygon_2` from the polylined outline projected through `frame`. Returns
// `std::nullopt` if the polygon is degenerate or self-intersecting (CGAL's offset would otherwise
// throw). Adjusts orientation to CCW because `create_interior_skeleton_and_offset_polygons_2`
// requires it.
std::optional<SkeletonPolygon> BuildProjectedPolygon(const std::vector<glm::dvec3> &points3d,
                                                    const FaceFrame &frame)
{
    if (points3d.size() < 3)
        return std::nullopt;
    SkeletonPolygon poly;
    poly.container().reserve(points3d.size());
    for (const glm::dvec3 &p3 : points3d)
    {
        const glm::dvec2 p2 = Project(p3, frame);
        poly.push_back(SkeletonPoint(p2.x, p2.y));
    }
    if (poly.size() < 3 || !poly.is_simple())
        return std::nullopt;
    if (poly.is_clockwise_oriented())
        poly.reverse_orientation();
    return poly;
}

// Runs CGAL's interior straight-skeleton + offset on `polygon` at distance `insetMm`, copying each
// produced offset polygon out so callers don't have to pull `boost::shared_ptr` into their type
// surface. The output vector holds one offset polygon per disconnected island produced by the
// inset (most planar faces produce exactly one island; concave faces can split). Returns an empty
// vector when the inset distance is large enough to consume the polygon entirely — caller treats
// that as "no inset emitted, only the outer ring."
std::vector<SkeletonPolygon> ComputeOffsetPolygons(const SkeletonPolygon &polygon, double insetMm)
{
    std::vector<SkeletonPolygon> out;
    const auto offsets = CGAL::create_interior_skeleton_and_offset_polygons_2(insetMm, polygon);
    out.reserve(offsets.size());
    for (const auto &p : offsets)
        if (p)
            out.push_back(*p);
    return out;
}

#endif // CAD_USE_CGAL

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

    SegmentList segments;
    const PolylinedOutline outline = BuildPolylinedOuterLoop(*face, params.chordTolMm);
    if (outline.points.size() < 3)
    {
        cache.emplace(key, segments);
        return segments;
    }

    const FaceFrame frame = BuildFaceFrame(*face, outline.points.front());

    // Always emit the outer ring so the user sees which face is being processed even when the
    // inset call fails (degenerate polygon, inset >= min in-radius, CGAL disabled).
    AppendRingAsSegments(outline.points, segments);

#if defined(CAD_USE_CGAL)
    if (auto polygonOpt = BuildProjectedPolygon(outline.points, frame); polygonOpt.has_value())
    {
        std::vector<SkeletonPolygon> offsets;
        // CGAL's offset routine throws on a handful of degenerate-but-`is_simple` inputs (notably
        // polygons with collinear consecutive edges). Swallow the exception, fall back to "outer
        // ring only" instead of crashing the tool.
        try
        {
            offsets = ComputeOffsetPolygons(*polygonOpt, params.insetMm);
        }
        catch (...)
        {
            offsets.clear();
        }
        for (const SkeletonPolygon &off : offsets)
        {
            std::vector<glm::dvec3> ring;
            ring.reserve(off.size());
            for (auto it = off.vertices_begin(); it != off.vertices_end(); ++it)
                ring.push_back(Unproject(glm::dvec2(CGAL::to_double(it->x()),
                                                    CGAL::to_double(it->y())),
                                          frame));
            AppendRingAsSegments(ring, segments);
        }
    }
#endif // CAD_USE_CGAL

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
