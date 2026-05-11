#include "Structure/StructureTriangulation.hpp"

#include "Geometry/Curve.hpp"
#include "Geometry/Edge.hpp"
#include "Geometry/Face.hpp"
#include "Geometry/OrientedEdge.hpp"
#include "Geometry/Point.hpp"
#include "Geometry/Surface.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <unordered_map>

#if defined(CAD_USE_CGAL)
#include <CGAL/Boolean_set_operations_2.h>
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Polygon_2.h>
#include <CGAL/Polygon_with_holes_2.h>
#include <CGAL/create_offset_polygons_2.h>

#include <list>
#endif

// See documentation/implementations/structure_face_triangulation_2026-05-11.md for phase notes.
// B2a: CMake flag rename, module scaffold, cache key. B2b: adaptive polyline of the face's outer
// loop with corner-vertex tags, orthonormal 2D frame for the face plane. B2c: CGAL straight-
// skeleton inset on top of the 2D polygon. B2d: corner-pair chord selection + strip rectangle.
// B2e: fillet helper (arc sampling at radius `insetMm`, chord-tolerant). B2-carve (current):
// reordered pipeline — `CGAL::difference(inset, strip)` first, fillet the carve result. B2f:
// panel slider + cache invalidation on drag.

namespace StructureTriangulation
{
namespace
{

using SegmentList = std::vector<std::pair<glm::vec3, glm::vec3>>;

#if defined(CAD_USE_CGAL)
using SkeletonKernel = CGAL::Exact_predicates_inexact_constructions_kernel;
using SkeletonPoint = SkeletonKernel::Point_2;
using SkeletonPolygon = CGAL::Polygon_2<SkeletonKernel>;
using SkeletonPolygonWithHoles = CGAL::Polygon_with_holes_2<SkeletonKernel>;
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

// Picks the segment count for an arc of `radius` sweeping `sweepRad`, sized so chord deviation
// stays at or under `chordTolMm`. Standard sagitta formula: `deviation = radius * (1 - cos(α/2))`
// where `α` is the per-segment angle. Floors at 2 segments so even tiny arcs don't degenerate to
// a single straight line; clamps the acos argument so radii barely larger than the tolerance
// don't blow up at the boundary.
int ArcSegmentCount(double sweepRad, double radius, double chordTolMm)
{
    if (radius <= 0.0)
        return 2;
    const double ratio = std::clamp(1.0 - chordTolMm / radius, -1.0, 1.0);
    const double alphaMax = 2.0 * std::acos(ratio);
    if (alphaMax <= 1e-9)
        return 2;
    return std::max(2, static_cast<int>(std::ceil(std::abs(sweepRad) / alphaMax)));
}

// Returns a new closed polyline ring with each convex corner of `ring` replaced by an arc-sampled
// fillet of radius `radius`. Reflex corners (interior angle ≥ 180°) and corners where the fillet
// would consume more than half of either adjacent edge length are passed through unchanged — the
// goal is "round corners where it fits, leave them alone where it doesn't" rather than to insist
// on a fillet everywhere. Operates in 2D so callers can unproject the result through any frame.
//
// Assumes `ring` is CCW (matches the post-`reverse_orientation` straight-skeleton output). For a
// CCW convex corner the inward bisector is `normalize(-inDir + outDir)`, the fillet center is at
// `radius / sin(θ/2)` along that bisector, and the arc sweeps CCW from the incoming-edge tangent
// to the outgoing-edge tangent.
std::vector<glm::dvec2> FilletPolygonCorners(const std::vector<glm::dvec2> &ring, double radius,
                                             double chordTolMm)
{
    const int n = static_cast<int>(ring.size());
    if (n < 3 || radius <= 0.0)
        return ring;

    std::vector<glm::dvec2> out;
    out.reserve(static_cast<std::size_t>(n) * 6);

    for (int i = 0; i < n; ++i)
    {
        const glm::dvec2 &V = ring[i];
        const glm::dvec2 &prev = ring[(i + n - 1) % n];
        const glm::dvec2 &next = ring[(i + 1) % n];

        const glm::dvec2 inEdge = V - prev;
        const glm::dvec2 outEdge = next - V;
        const double inLen = glm::length(inEdge);
        const double outLen = glm::length(outEdge);
        if (inLen < 1e-12 || outLen < 1e-12)
        {
            out.push_back(V);
            continue;
        }
        const glm::dvec2 inDir = inEdge / inLen;
        const glm::dvec2 outDir = outEdge / outLen;
        const double cross = inDir.x * outDir.y - inDir.y * outDir.x;
        // CCW polygon: positive cross at a convex corner. Treat near-zero (collinear) as "no
        // corner to round" so we don't try to evaluate `tan(π/2)`.
        if (cross <= 1e-9)
        {
            out.push_back(V);
            continue;
        }
        const double dot = inDir.x * outDir.x + inDir.y * outDir.y;
        const double turn = std::atan2(cross, dot);
        const double theta = M_PI - turn;
        const double halfTheta = 0.5 * theta;
        const double tanHalf = std::tan(halfTheta);
        const double sinHalf = std::sin(halfTheta);
        if (tanHalf < 1e-9 || sinHalf < 1e-9)
        {
            out.push_back(V);
            continue;
        }
        const double d = radius / tanHalf;
        if (d >= 0.5 * inLen || d >= 0.5 * outLen)
        {
            // Fillet doesn't fit without overlapping the adjacent corner's fillet on the same edge.
            out.push_back(V);
            continue;
        }
        const glm::dvec2 fStart = V - inDir * d;
        const glm::dvec2 fEnd = V + outDir * d;
        const glm::dvec2 bisDir = glm::normalize(-inDir + outDir);
        const glm::dvec2 center = V + bisDir * (radius / sinHalf);

        const glm::dvec2 startVec = fStart - center;
        const glm::dvec2 endVec = fEnd - center;
        const double startAngle = std::atan2(startVec.y, startVec.x);
        const double endAngle = std::atan2(endVec.y, endVec.x);
        double sweep = endAngle - startAngle;
        if (sweep < 0.0)
            sweep += 2.0 * M_PI;
        int nSeg = ArcSegmentCount(sweep, radius, chordTolMm);
        // Visual-smoothness floor: keep the per-segment angle at most ~11.25° regardless of how
        // generous the chord tolerance is. Sagitta-based tolerance alone isn't enough at small
        // radii (a 2 mm fillet at 0.1 mm tolerance would produce only 3 segments per quarter-arc
        // — readable as a chamfer, not a curve). Floors at 8 segments per quarter-arc; larger
        // radii that already exceed the floor under their tolerance budget keep the tighter count.
        const int floorSegs = static_cast<int>(std::ceil(sweep / (M_PI / 16.0)));
        nSeg = std::max(nSeg, floorSegs);

        out.push_back(fStart);
        for (int k = 1; k < nSeg; ++k)
        {
            const double t = static_cast<double>(k) / static_cast<double>(nSeg);
            const double a = startAngle + t * sweep;
            out.emplace_back(center.x + radius * std::cos(a),
                             center.y + radius * std::sin(a));
        }
        out.push_back(fEnd);
    }
    return out;
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

// Builds the strip rectangle as a CCW `Polygon_2` ready for boolean operations against the inset.
// Vertex order is SW → SE → NE → NW where "north" is `perp` (the 90° CCW rotation of the chord
// direction). Returns an empty polygon (size 0) when the chord is degenerate so callers can fall
// back to "inset only" instead of feeding CGAL a malformed input.
SkeletonPolygon BuildStripPolygonCCW(const SkeletonPolygon &off, int aIdx, int bIdx, double widthMm)
{
    SkeletonPolygon poly;
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
        return poly;
    dir /= len;
    const glm::dvec2 perp(-dir.y, dir.x);
    const double hw = 0.5 * widthMm;
    const glm::dvec2 sw = a - perp * hw;
    const glm::dvec2 se = b - perp * hw;
    const glm::dvec2 ne = b + perp * hw;
    const glm::dvec2 nw = a + perp * hw;
    poly.push_back(SkeletonPoint(sw.x, sw.y));
    poly.push_back(SkeletonPoint(se.x, se.y));
    poly.push_back(SkeletonPoint(ne.x, ne.y));
    poly.push_back(SkeletonPoint(nw.x, nw.y));
    return poly;
}

// Pulls a 2D ring out of a CGAL `Polygon_2`. Convenience helper so the carve loop doesn't repeat
// the same `vertices_begin / to_double` boilerplate for outer boundaries and hole boundaries.
std::vector<glm::dvec2> ExtractRing2D(const SkeletonPolygon &poly)
{
    std::vector<glm::dvec2> out;
    out.reserve(poly.size());
    for (auto it = poly.vertices_begin(); it != poly.vertices_end(); ++it)
        out.emplace_back(CGAL::to_double(it->x()), CGAL::to_double(it->y()));
    return out;
}

// Unprojects a 2D ring back through `frame` and pushes it onto `out` as consecutive line-segment
// pairs (closed loop). The fillet result feeds straight through this without rewriting the
// 2D-vs-3D boundary again.
void EmitRing2D(const std::vector<glm::dvec2> &ring2D, const FaceFrame &frame, SegmentList &out)
{
    std::vector<glm::dvec3> ring3D;
    ring3D.reserve(ring2D.size());
    for (const glm::dvec2 &p : ring2D)
        ring3D.push_back(Unproject(p, frame));
    AppendRingAsSegments(ring3D, out);
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

// Set `CAD_DEBUG_STRUCTURE=1` to print one line per offset island when the strip/carve path runs
// (stderr). Intended for diagnosing missing strips without enabling global verbose logging.
bool EnvStructureStripDebugOn()
{
    const char *e = std::getenv("CAD_DEBUG_STRUCTURE");
    return e != nullptr && e[0] != '\0' && e[0] != '0';
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
        if (EnvStructureStripDebugOn())
        {
            const std::size_t nCorners =
                static_cast<std::size_t>(std::count(outline.isCorner.begin(), outline.isCorner.end(), true));
            std::cerr << "[StructureTriangulation] bake face=" << static_cast<const void *>(face)
                      << " outlineVerts=" << outline.points.size() << " brpCorners=" << nCorners
                      << " polyVerts=" << polyVertexCount << " cornerPolyIdx=" << cornerPolyIndices.size()
                      << " insetMm=" << params.insetMm << " offsetIslands=" << offsets.size() << '\n';
        }
        std::size_t offsetIsland = 0;
        for (const SkeletonPolygon &off : offsets)
        {
            // Step 1: chord selection on the *sharp* offset polygon. The strip anchors at the
            // original CGAL inset corners — anchoring to fillet-shifted positions would walk the
            // strip inwards. Strip lookup is gated on the convex-preservation check (matching
            // vertex counts between input polygon and offset polygon); concave faces fall through
            // to "no strip, emit filleted inset only."
            const int offVertexCount = static_cast<int>(off.size());
            const bool vertexCountGuard = (offVertexCount == polyVertexCount);
            std::optional<ChordPick> chord;
            if (vertexCountGuard)
                chord = SelectFirstValidStripChord(off, cornerPolyIndices);

            const char *stripReject = nullptr;
            bool differenceThrew = false;
            std::size_t piecesAfterDifference = 0;

            // Step 2: 2D boolean carve. `inset \ strip` produces the actual triangulated sub-
            // regions whose boundaries the user will see. Without a chord (no valid strip) we
            // treat the whole offset polygon as the single carve result. CGAL's difference can
            // throw on degenerate geometry, so on any exception we fall back to inset-only.
            std::list<SkeletonPolygonWithHoles> carved;
            if (chord.has_value())
            {
                const SkeletonPolygon strip =
                    BuildStripPolygonCCW(off, chord->aIdx, chord->bIdx, params.insetMm);
                if (strip.size() == 4 && strip.is_simple())
                {
                    try
                    {
                        CGAL::difference(off, strip, std::back_inserter(carved));
                        piecesAfterDifference = carved.size();
                    }
                    catch (...)
                    {
                        differenceThrew = true;
                        carved.clear();
                    }
                }
                else
                    stripReject = (strip.size() != 4) ? "strip_not_quad" : "strip_not_simple";
            }
            const bool usedInsetFallback = carved.empty();
            if (usedInsetFallback)
                carved.emplace_back(off);

            if (EnvStructureStripDebugOn())
            {
                std::ostringstream line;
                line << "[StructureTriangulation] island=" << offsetIsland << " offVerts=" << offVertexCount
                     << " vertexGuard=" << (vertexCountGuard ? "pass" : "fail")
                     << " chord=" << (chord.has_value() ? "yes" : "no");
                if (chord.has_value())
                    line << " a=" << chord->aIdx << " b=" << chord->bIdx;
                if (stripReject != nullptr)
                    line << " strip=" << stripReject;
                if (differenceThrew)
                    line << " cgal_difference=threw";
                else if (chord.has_value() && stripReject == nullptr)
                    line << " piecesAfterDiff=" << piecesAfterDifference;
                line << " carvedPieces=" << carved.size() << " insetFallback=" << (usedInsetFallback ? "yes" : "no");
                std::cerr << line.str() << '\n';
            }
            ++offsetIsland;

            // Step 3: fillet then emit each carve-result boundary. Outer boundaries are CCW out
            // of CGAL — feed `FilletPolygonCorners` directly. Hole boundaries are CW (CGAL
            // convention); reverse to CCW before filleting so the helper's "convex corner" check
            // (positive cross product) lines up with the hole's visible-convex corners.
            for (const SkeletonPolygonWithHoles &pwh : carved)
            {
                const std::vector<glm::dvec2> outer2D = ExtractRing2D(pwh.outer_boundary());
                const std::vector<glm::dvec2> filletedOuter =
                    FilletPolygonCorners(outer2D, params.insetMm, params.chordTolMm);
                EmitRing2D(filletedOuter, frame, segments);

                for (auto holeIt = pwh.holes_begin(); holeIt != pwh.holes_end(); ++holeIt)
                {
                    std::vector<glm::dvec2> hole2D = ExtractRing2D(*holeIt);
                    std::reverse(hole2D.begin(), hole2D.end());
                    const std::vector<glm::dvec2> filletedHole =
                        FilletPolygonCorners(hole2D, params.insetMm, params.chordTolMm);
                    EmitRing2D(filletedHole, frame, segments);
                }
            }
        }
    }
    else if (EnvStructureStripDebugOn())
    {
        std::cerr << "[StructureTriangulation] bake face=" << static_cast<const void *>(face)
                  << " BuildProjectedPolygon failed (degenerate or not simple in 2D)\n";
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
