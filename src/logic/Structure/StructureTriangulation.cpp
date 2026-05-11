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
// B2a: CMake flag rename, module scaffold, cache key. B2b: adaptive polyline of the face's outer
// loop with corner-vertex tags, orthonormal 2D frame for the face plane. B2c: CGAL straight-
// skeleton inset on top of the 2D polygon. B2d (current): corner-pair chord selection + strip-
// band emission across the inset polygon. B2e: fillet inset ring corners. B2f: panel slider +
// cache invalidation on drag.

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

// Result of projecting a 3D outline into the face plane and building a CGAL `Polygon_2`. The
// `reversed` flag records whether the input outline was clockwise in 2D (we flip to CCW because
// `create_interior_skeleton_and_offset_polygons_2` requires it) — downstream code that maps
// outline-vertex indices to polygon indices must consult this flag, otherwise corner labels
// land on the wrong vertices.
struct ProjectedPolygon
{
    SkeletonPolygon polygon;
    bool reversed = false;
};

// Builds a CGAL `Polygon_2` from the polylined outline projected through `frame`. Returns
// `std::nullopt` if the polygon is degenerate or self-intersecting (CGAL's offset would otherwise
// throw).
std::optional<ProjectedPolygon> BuildProjectedPolygon(const std::vector<glm::dvec3> &points3d,
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
    ProjectedPolygon out;
    out.polygon = std::move(poly);
    if (out.polygon.is_clockwise_oriented())
    {
        out.polygon.reverse_orientation();
        out.reversed = true;
    }
    return out;
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

// Maps "is this outline vertex a B-rep corner?" flags to polygon-vertex indices, after accounting
// for the CCW reversal `BuildProjectedPolygon` may have applied. When the outline was reversed,
// outline index `k` lives at polygon index `(n - 1 - k)`; we sort the result so callers can treat
// the returned indices as ascending polygon-order corner positions either way. The straight-
// skeleton inset preserves vertex count and order for convex inputs, so the same indices apply
// to the offset polygon — non-convex faces will fail the vertex-count guard in the caller.
std::vector<int> MapCornerIndicesToPoly(const std::vector<bool> &isCorner, bool reversed)
{
    std::vector<int> out;
    const int n = static_cast<int>(isCorner.size());
    out.reserve(static_cast<std::size_t>(std::count(isCorner.begin(), isCorner.end(), true)));
    for (int k = 0; k < n; ++k)
    {
        if (!isCorner[k])
            continue;
        out.push_back(reversed ? (n - 1 - k) : k);
    }
    if (reversed)
        std::sort(out.begin(), out.end());
    return out;
}

// Chord pick: indices into the offset polygon plus the squared chord length used for sorting.
// Squared length keeps the comparator branch-free for ties (two chords of exactly the same length
// fall through to a deterministic index-based tiebreak, important for stable visuals across runs).
struct ChordPick
{
    int aIdx = 0;
    int bIdx = 0;
    double lengthSq = 0.0;
};

// Enumerates every non-adjacent corner-pair chord on the offset polygon, keeps the ones whose
// midpoint lies inside the polygon (the cheap proxy for "chord stays inside" — exact for convex
// polygons, defensible for now on non-convex too because we already gate on convex preservation
// upstream), and returns the deterministic winner: shortest first, lowest start-index next,
// lowest end-index last. The polygon ordering is CCW after `BuildProjectedPolygon`, so adjacency
// is "consecutive modulo `n`".
//
// Tiebreaker note: a 4-corner square has two equally long diagonals; the index tiebreak picks
// the diagonal anchored at the lowest-index corner. Replaceable with a user-choice gesture
// when we get to interactive chord picking.
std::optional<ChordPick> SelectFirstValidStripChord(const SkeletonPolygon &off,
                                                    const std::vector<int> &cornerPolyIndices)
{
    const int n = static_cast<int>(off.size());
    const int c = static_cast<int>(cornerPolyIndices.size());
    // Need at least 4 distinct corners to have a non-adjacent pair (3-corner polygons have only
    // adjacent pairs; 2 / fewer obviously can't form a chord).
    if (c < 4 || n < 4)
        return std::nullopt;

    std::vector<glm::dvec2> off2D;
    off2D.reserve(static_cast<std::size_t>(n));
    for (auto it = off.vertices_begin(); it != off.vertices_end(); ++it)
        off2D.emplace_back(CGAL::to_double(it->x()), CGAL::to_double(it->y()));

    std::vector<ChordPick> candidates;
    for (int i = 0; i < c; ++i)
    {
        const int aIdx = cornerPolyIndices[i];
        if (aIdx < 0 || aIdx >= n)
            continue;
        for (int j = i + 1; j < c; ++j)
        {
            const int bIdx = cornerPolyIndices[j];
            if (bIdx < 0 || bIdx >= n)
                continue;
            // Chord coincides with a polygon edge when the two corners are CCW-adjacent.
            if (((aIdx + 1) % n) == bIdx || ((bIdx + 1) % n) == aIdx)
                continue;
            const glm::dvec2 &a = off2D[aIdx];
            const glm::dvec2 &b = off2D[bIdx];
            const glm::dvec2 mid = 0.5 * (a + b);
            const auto side = off.bounded_side(SkeletonPoint(mid.x, mid.y));
            if (side != CGAL::ON_BOUNDED_SIDE)
                continue;
            const double dx = b.x - a.x;
            const double dy = b.y - a.y;
            candidates.push_back({aIdx, bIdx, dx * dx + dy * dy});
        }
    }
    if (candidates.empty())
        return std::nullopt;
    std::sort(candidates.begin(), candidates.end(),
              [](const ChordPick &A, const ChordPick &B)
              {
                  if (A.lengthSq != B.lengthSq)
                      return A.lengthSq < B.lengthSq;
                  if (A.aIdx != B.aIdx)
                      return A.aIdx < B.aIdx;
                  return A.bIdx < B.bIdx;
              });
    return candidates.front();
}

// Emits the strip band as a closed 4-segment rectangle (two long edges parallel to the chord at
// offset `±widthMm/2`, two short caps perpendicular to the chord at each endpoint). The endpoint
// caps live at the inset-polygon corners themselves; for sharp corners they can poke a fraction
// outside the inset polygon, which the eventual 2D union (a later phase) will fix. For B2d's
// preview-only output the small protrusion is acceptable — the inset ring is drawn first, so the
// visible overlap reads as "strip touches the corner" without misrepresenting the carve shape.
void EmitStripBand(const SkeletonPolygon &off, int aIdx, int bIdx, double widthMm,
                   const FaceFrame &frame, SegmentList &out)
{
    const auto vert = [&](int idx) -> glm::dvec2
    {
        const auto &p = off.vertex(idx);
        return {CGAL::to_double(p.x()), CGAL::to_double(p.y())};
    };
    const glm::dvec2 a = vert(aIdx);
    const glm::dvec2 b = vert(bIdx);
    glm::dvec2 dir = b - a;
    const double len = glm::length(dir);
    if (len < 1e-9)
        return;
    dir /= len;
    const glm::dvec2 perp(-dir.y, dir.x);
    const double hw = 0.5 * widthMm;
    const std::vector<glm::dvec3> ring = {
        Unproject(a + perp * hw, frame),
        Unproject(b + perp * hw, frame),
        Unproject(b - perp * hw, frame),
        Unproject(a - perp * hw, frame),
    };
    AppendRingAsSegments(ring, out);
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
    if (auto projected = BuildProjectedPolygon(outline.points, frame); projected.has_value())
    {
        std::vector<SkeletonPolygon> offsets;
        // CGAL's offset routine throws on a handful of degenerate-but-`is_simple` inputs (notably
        // polygons with collinear consecutive edges). Swallow the exception, fall back to "outer
        // ring only" instead of crashing the tool.
        try
        {
            offsets = ComputeOffsetPolygons(projected->polygon, params.insetMm);
        }
        catch (...)
        {
            offsets.clear();
        }
        const std::vector<int> cornerPolyIndices =
            MapCornerIndicesToPoly(outline.isCorner, projected->reversed);
        const int polyVertexCount = static_cast<int>(projected->polygon.size());
        for (const SkeletonPolygon &off : offsets)
        {
            std::vector<glm::dvec3> ring;
            ring.reserve(off.size());
            for (auto it = off.vertices_begin(); it != off.vertices_end(); ++it)
                ring.push_back(Unproject(glm::dvec2(CGAL::to_double(it->x()),
                                                    CGAL::to_double(it->y())),
                                          frame));
            AppendRingAsSegments(ring, segments);
            // B2d strip emission. Vertex-count match implies the straight skeleton preserved every
            // original polygon vertex (true for convex inputs at the inset distances we expect).
            // Non-matching count (collapsed reflex vertices on concave faces, or distance-induced
            // smoothing) leaves the corner labels unaligned with offset vertices, so we skip the
            // strip rather than emit a mis-anchored chord. Concave handling lives in a later phase.
            if (static_cast<int>(off.size()) != polyVertexCount)
                continue;
            if (auto chord = SelectFirstValidStripChord(off, cornerPolyIndices))
                EmitStripBand(off, chord->aIdx, chord->bIdx, params.insetMm, frame, segments);
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
