#include "GeometryValidity.hpp"

#include "Geometry/Edge.hpp"
#include "Geometry/Face.hpp"
#include "Geometry/OrientedEdge.hpp"
#include "Geometry/Point.hpp"
#include "Geometry/Solid.hpp"
#include "Geometry/Surface.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GeometryValidity
{
namespace
{

[[nodiscard]] double TriangleDoubledAreaSquared(const glm::dvec3 &a, const glm::dvec3 &b,
                                                const glm::dvec3 &c) noexcept
{
    const glm::dvec3 cr = glm::cross(b - a, c - a);
    return glm::dot(cr, cr);
}

[[nodiscard]] double BboxMaxExtent(const glm::dvec3 &mn, const glm::dvec3 &mx) noexcept
{
    const glm::dvec3 ext = mx - mn;
    return std::max({ext.x, ext.y, ext.z, 0.0});
}

struct SolidAabb
{
    glm::dvec3 mn{std::numeric_limits<double>::max()};
    glm::dvec3 mx{-std::numeric_limits<double>::max()};
    bool any = false;
};

[[nodiscard]] SolidAabb ComputeSolidPointBounds(const Solid &solid) noexcept
{
    SolidAabb box;
    for (const Face *face : solid.faces)
    {
        if (face == nullptr)
            continue;
        for (const auto &loop : face->loops)
        {
            for (const OrientedEdge &oe : loop)
            {
                if (oe.edge == nullptr)
                    continue;
                Point *a = oe.GetStart();
                Point *b = oe.GetEnd();
                if (a != nullptr)
                {
                    box.any = true;
                    box.mn = glm::min(box.mn, a->position);
                    box.mx = glm::max(box.mx, a->position);
                }
                if (b != nullptr)
                {
                    box.any = true;
                    box.mn = glm::min(box.mn, b->position);
                    box.mx = glm::max(box.mx, b->position);
                }
            }
        }
    }
    return box;
}

[[nodiscard]] double MinFanTriangleAreaSquaredThreshold(const Solid &solid) noexcept
{
    const SolidAabb box = ComputeSolidPointBounds(solid);
    if (!box.any)
        return 1e-36;
    const double extent = BboxMaxExtent(box.mn, box.mx);
    const double scale = std::max(extent, 1.0);
    return std::max(1e-36, (1e-24 * scale) * (1e-24 * scale));
}

[[nodiscard]] double WeldEpsilonFromSolid(const Solid &solid) noexcept
{
    const SolidAabb box = ComputeSolidPointBounds(solid);
    if (!box.any)
        return 1e-12;
    const double maxAbsCoord = std::max({
        std::abs(box.mn.x), std::abs(box.mn.y), std::abs(box.mn.z),
        std::abs(box.mx.x), std::abs(box.mx.y), std::abs(box.mx.z),
        1.0});
    const double floatNoise = 2.0 * static_cast<double>(std::numeric_limits<float>::epsilon()) * maxAbsCoord * 16.0;
    const double geometric = 1e-9 * std::max(BboxMaxExtent(box.mn, box.mx), 1.0);
    return std::max({floatNoise, geometric, 1e-15});
}

struct WeldGridKey
{
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;
    bool operator==(const WeldGridKey &o) const noexcept { return x == o.x && y == o.y && z == o.z; }
};

struct WeldGridKeyHash
{
    std::size_t operator()(const WeldGridKey &k) const noexcept
    {
        std::size_t h = std::hash<std::int64_t>{}(k.x);
        h ^= std::hash<std::int64_t>{}(k.y) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<std::int64_t>{}(k.z) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

[[nodiscard]] WeldGridKey MakeWeldKey(const glm::dvec3 &pos, double weldEps) noexcept
{
    const auto q = [&](double c) -> std::int64_t
    {
        return static_cast<std::int64_t>(std::floor(c / weldEps));
    };
    return {q(pos.x), q(pos.y), q(pos.z)};
}

struct Dsu
{
    std::vector<int> parent;
    explicit Dsu(int n) : parent(static_cast<std::size_t>(n))
    {
        for (int i = 0; i < n; ++i)
            parent[static_cast<std::size_t>(i)] = i;
    }
    int find(int x) noexcept
    {
        int r = x;
        while (parent[static_cast<std::size_t>(r)] != r)
            r = parent[static_cast<std::size_t>(r)];
        while (parent[static_cast<std::size_t>(x)] != x)
        {
            const int nxt = parent[static_cast<std::size_t>(x)];
            parent[static_cast<std::size_t>(x)] = r;
            x = nxt;
        }
        return r;
    }
    void unite(int a, int b) noexcept
    {
        a = find(a);
        b = find(b);
        if (a != b)
            parent[static_cast<std::size_t>(b)] = a;
    }
};

void CollectSolidEdges(const Solid &solid, std::unordered_set<Edge *> &out) noexcept
{
    out.clear();
    for (const Face *face : solid.faces)
    {
        if (face == nullptr)
            continue;
        for (const auto &loop : face->loops)
        {
            for (const OrientedEdge &oe : loop)
            {
                if (oe.edge != nullptr)
                    out.insert(oe.edge);
            }
        }
    }
}

void MarkConstrainedEndpoints(const std::unordered_set<Edge *> &edges, std::unordered_set<Point *> &constrained) noexcept
{
    constrained.clear();
    for (Edge *e : edges)
    {
        if (e == nullptr)
            continue;
        if (e->curve != nullptr || !e->bridgePoints.empty())
        {
            if (e->startPoint != nullptr)
                constrained.insert(e->startPoint);
            if (e->endPoint != nullptr)
                constrained.insert(e->endPoint);
        }
    }
}

[[nodiscard]] bool FaceStillDegenerateByFanRule(const Face *face, double minArea2) noexcept
{
    if (face == nullptr || face->surface == nullptr || face->loops.empty())
        return false;
    for (const auto &loop : face->loops)
    {
        if (loop.empty())
            continue;
        std::vector<glm::dvec3> ring;
        ring.reserve(loop.size());
        for (const OrientedEdge &oe : loop)
        {
            if (oe.edge == nullptr || oe.GetStart() == nullptr)
                return false;
            ring.push_back(oe.GetStartPosition());
        }
        const std::size_t n = ring.size();
        if (n < 3)
            return true;
        if (n == 3)
            return TriangleDoubledAreaSquared(ring[0], ring[1], ring[2]) < minArea2;
        for (std::size_t i = 1; i + 1 < n; ++i)
        {
            if (TriangleDoubledAreaSquared(ring[0], ring[i], ring[i + 1]) < minArea2)
                return true;
        }
    }
    return false;
}

void RemoveFaceFromSolid(Solid &solid, Face *face) noexcept
{
    if (face == nullptr)
        return;
    for (auto &loop : face->loops)
    {
        for (auto &oe : loop)
        {
            if (oe.edge != nullptr)
                oe.edge->dependencies.erase(face);
        }
    }
    face->loops.clear();
    face->dependency = nullptr;
    solid.faces.erase(std::remove(solid.faces.begin(), solid.faces.end(), face), solid.faces.end());
}

[[nodiscard]] bool LexLessWeldKey(WeldGridKey a, WeldGridKey b) noexcept
{
    return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
}

struct UndirectedSegGridKey
{
    WeldGridKey k0{};
    WeldGridKey k1{};
    bool operator==(const UndirectedSegGridKey &o) const noexcept
    {
        return k0.x == o.k0.x && k0.y == o.k0.y && k0.z == o.k0.z && k1.x == o.k1.x && k1.y == o.k1.y && k1.z == o.k1.z;
    }
};

struct UndirectedSegGridKeyHash
{
    std::size_t operator()(const UndirectedSegGridKey &k) const noexcept
    {
        WeldGridKeyHash h{};
        std::size_t out = h(k.k0);
        out ^= h(k.k1) + 0x9e3779b97f4a7c15ULL + (out << 6) + (out >> 2);
        return out;
    }
};

[[nodiscard]] UndirectedSegGridKey MakeUndirectedSegGridKey(const glm::dvec3 &pa, const glm::dvec3 &pb,
                                                             double weldEps) noexcept
{
    WeldGridKey a = MakeWeldKey(pa, weldEps);
    WeldGridKey b = MakeWeldKey(pb, weldEps);
    if (LexLessWeldKey(b, a))
        std::swap(a, b);
    return {a, b};
}

[[nodiscard]] bool EdgesGeomCoincidentUndirected(const glm::dvec3 &a0, const glm::dvec3 &b0,
                                                 const glm::dvec3 &a1, const glm::dvec3 &b1,
                                                 double segTol) noexcept
{
    const double dAlign = glm::distance(a0, a1) + glm::distance(b0, b1);
    const double dCross = glm::distance(a0, b1) + glm::distance(b0, a1);
    return std::min(dAlign, dCross) <= segTol;
}

using DirectedFaceUse = std::tuple<Face *, Point *, Point *>;

void SortUniqueDirectedUsesInPlace(std::vector<DirectedFaceUse> &uses) noexcept
{
    std::sort(uses.begin(), uses.end(), [](const DirectedFaceUse &t1, const DirectedFaceUse &t2) noexcept
    {
        Face *f1 = std::get<0>(t1);
        Face *f2 = std::get<0>(t2);
        if (f1 != f2)
            return reinterpret_cast<std::uintptr_t>(f1) < reinterpret_cast<std::uintptr_t>(f2);
        Point *a1 = std::get<1>(t1);
        Point *a2 = std::get<1>(t2);
        if (a1 != a2)
            return reinterpret_cast<std::uintptr_t>(a1) < reinterpret_cast<std::uintptr_t>(a2);
        Point *b1 = std::get<2>(t1);
        Point *b2 = std::get<2>(t2);
        return reinterpret_cast<std::uintptr_t>(b1) < reinterpret_cast<std::uintptr_t>(b2);
    });
    uses.erase(std::unique(uses.begin(), uses.end(),
                           [](const DirectedFaceUse &t1, const DirectedFaceUse &t2) noexcept
                           {
                               return std::get<0>(t1) == std::get<0>(t2) && std::get<1>(t1) == std::get<1>(t2) &&
                                      std::get<2>(t1) == std::get<2>(t2);
                           }),
               uses.end());
}

void AppendTagsFromDirectedUses(std::vector<DirectedFaceUse> &uses, AppInvalidTag &tags) noexcept
{
    SortUniqueDirectedUsesInPlace(uses);

    const int c = static_cast<int>(uses.size());
    if (c == 1)
        tags |= AppInvalidTag::OpenBoundary;
    else if (c > 2)
        tags |= AppInvalidTag::NonManifoldConnectivity;
    else if (c == 2)
    {
        Point *a0 = std::get<1>(uses[0]);
        Point *b0 = std::get<2>(uses[0]);
        Point *a1 = std::get<1>(uses[1]);
        Point *b1 = std::get<2>(uses[1]);
        const bool opposite = (a0 == b1 && b0 == a1);
        const bool same = (a0 == a1 && b0 == b1);
        if (same)
            tags |= AppInvalidTag::InconsistentFaceOrientation;
        else if (!opposite)
            tags |= AppInvalidTag::NonManifoldConnectivity;
    }
}

void EvaluateEdgeConnectivityTags(
    const Solid &solid,
    const std::unordered_map<Edge *, std::vector<std::tuple<Face *, Point *, Point *>>> &edgeUses,
    AppInvalidTag &tags,
    std::vector<const Edge *> *openBoundaryEdgesOut) noexcept
{
    for (const auto &kv : edgeUses)
    {
        Edge *const e = kv.first;
        if (e == nullptr)
            continue;
        if (e->curve != nullptr || !e->bridgePoints.empty())
        {
            std::vector<std::tuple<Face *, Point *, Point *>> copy = kv.second;
            if (openBoundaryEdgesOut != nullptr)
            {
                std::vector<DirectedFaceUse> tmp = kv.second;
                SortUniqueDirectedUsesInPlace(tmp);
                if (tmp.size() == 1u)
                    openBoundaryEdgesOut->push_back(e);
            }
            AppendTagsFromDirectedUses(copy, tags);
        }
    }

    struct StraightGeom
    {
        Edge *edge = nullptr;
        glm::dvec3 pa{};
        glm::dvec3 pb{};
    };

    std::vector<StraightGeom> straight;
    straight.reserve(edgeUses.size());
    for (const auto &kv : edgeUses)
    {
        Edge *const e = kv.first;
        if (e == nullptr || e->curve != nullptr || !e->bridgePoints.empty())
            continue;
        if (e->startPoint == nullptr || e->endPoint == nullptr)
            continue;
        straight.push_back({e, e->startPoint->position, e->endPoint->position});
    }

    const int nStraight = static_cast<int>(straight.size());
    if (nStraight == 0)
        return;

    const double weldEps = WeldEpsilonFromSolid(solid);
    const double segTol = std::max(8.0 * weldEps, 1e-15);

    Dsu dsu(nStraight);
    std::unordered_map<UndirectedSegGridKey, std::vector<int>, UndirectedSegGridKeyHash> buckets;
    buckets.reserve(static_cast<std::size_t>(nStraight) * 2u + 8u);
    for (int i = 0; i < nStraight; ++i)
    {
        const StraightGeom &sg = straight[static_cast<std::size_t>(i)];
        const UndirectedSegGridKey key = MakeUndirectedSegGridKey(sg.pa, sg.pb, weldEps);
        buckets[key].push_back(i);
    }

    for (auto &bk : buckets)
    {
        std::vector<int> &ix = bk.second;
        for (std::size_t a = 0; a < ix.size(); ++a)
        {
            for (std::size_t b = a + 1; b < ix.size(); ++b)
            {
                const StraightGeom &sa = straight[static_cast<std::size_t>(ix[a])];
                const StraightGeom &sb = straight[static_cast<std::size_t>(ix[b])];
                if (EdgesGeomCoincidentUndirected(sa.pa, sa.pb, sb.pa, sb.pb, segTol))
                    dsu.unite(ix[static_cast<std::size_t>(a)], ix[static_cast<std::size_t>(b)]);
            }
        }
    }

    std::unordered_map<int, std::vector<int>> groups;
    groups.reserve(static_cast<std::size_t>(nStraight) * 2u + 8u);
    for (int i = 0; i < nStraight; ++i)
        groups[dsu.find(i)].push_back(i);

    for (const auto &gv : groups)
    {
        std::vector<std::tuple<Face *, Point *, Point *>> merged;
        merged.reserve(gv.second.size() * 4u + 8u);
        for (int idx : gv.second)
        {
            Edge *const e = straight[static_cast<std::size_t>(idx)].edge;
            const auto it = edgeUses.find(e);
            if (it != edgeUses.end())
                merged.insert(merged.end(), it->second.begin(), it->second.end());
        }
        if (openBoundaryEdgesOut != nullptr)
        {
            std::vector<DirectedFaceUse> tmp = merged;
            SortUniqueDirectedUsesInPlace(tmp);
            if (tmp.size() == 1u && !gv.second.empty())
                openBoundaryEdgesOut->push_back(straight[static_cast<std::size_t>(gv.second[0])].edge);
        }
        AppendTagsFromDirectedUses(merged, tags);
    }
}

struct PointPairHash
{
    std::size_t operator()(const std::pair<Point *, Point *> &p) const noexcept
    {
        return std::hash<void *>{}(p.first) ^ (std::hash<void *>{}(p.second) << 1);
    }
};

void EvaluateVertexLinkConnectivityTags(const Solid &solid, AppInvalidTag &tags) noexcept
{
    std::unordered_set<Point *> seenPoints;
    seenPoints.reserve(solid.faces.size() * 4u + 8u);

    for (const Face *face : solid.faces)
    {
        if (face == nullptr)
            continue;
        for (const auto &loop : face->loops)
        {
            for (const OrientedEdge &oe : loop)
            {
                if (oe.edge == nullptr)
                    continue;
                if (Point *s = oe.GetStart())
                    seenPoints.insert(s);
                if (Point *e = oe.GetEnd())
                    seenPoints.insert(e);
            }
        }
    }

    for (Point *p : seenPoints)
    {
        if (p == nullptr)
            continue;

        [&]() {
            std::vector<std::pair<Point *, Point *>> linkEdges;
            linkEdges.reserve(32u);

            for (const Face *face : solid.faces)
            {
                if (face == nullptr)
                    continue;
                for (const auto &loop : face->loops)
                {
                    const std::size_t n = loop.size();
                    if (n < 3)
                        continue;
                    for (std::size_t j = 0; j < n; ++j)
                    {
                        const OrientedEdge &oe = loop[j];
                        if (oe.edge == nullptr || oe.GetStart() == nullptr || oe.GetEnd() == nullptr)
                            continue;
                        if (oe.GetStart() != p)
                            continue;
                        const std::size_t jPrev = (j + n - 1) % n;
                        Point *const prevN = loop[jPrev].GetStart();
                        Point *const nextN = oe.GetEnd();
                        if (prevN == nullptr || nextN == nullptr)
                            continue;
                        if (prevN == p || nextN == p || prevN == nextN)
                            continue;
                        Point *lo = prevN;
                        Point *hi = nextN;
                        if (reinterpret_cast<std::uintptr_t>(lo) > reinterpret_cast<std::uintptr_t>(hi))
                            std::swap(lo, hi);
                        linkEdges.emplace_back(lo, hi);
                    }
                }
            }

            if (linkEdges.empty())
                return;

            std::unordered_map<std::pair<Point *, Point *>, int, PointPairHash> wedgeCount;
            wedgeCount.reserve(linkEdges.size() * 2u + 8u);
            for (const auto &pr : linkEdges)
            {
                if (++wedgeCount[pr] > 1)
                {
                    tags |= AppInvalidTag::NonManifoldConnectivity;
                    return;
                }
            }

            std::unordered_map<Point *, std::unordered_set<Point *>> adj;
            adj.reserve(wedgeCount.size() * 4u + 8u);
            for (const auto &kv : wedgeCount)
            {
                Point *u = kv.first.first;
                Point *v = kv.first.second;
                adj[u].insert(v);
                adj[v].insert(u);
            }

            for (const auto &kv : adj)
            {
                if (kv.second.size() > 2u)
                {
                    tags |= AppInvalidTag::NonManifoldConnectivity;
                    return;
                }
            }

            std::unordered_set<Point *> nodes;
            for (const auto &kv : wedgeCount)
            {
                nodes.insert(kv.first.first);
                nodes.insert(kv.first.second);
            }

            std::unordered_set<Point *> visitedGlobal;
            int componentsWithEdges = 0;
            for (Point *seed : nodes)
            {
                if (visitedGlobal.count(seed) != 0u)
                    continue;
                std::unordered_set<Point *> comp;
                std::queue<Point *> q;
                q.push(seed);
                comp.insert(seed);
                while (!q.empty())
                {
                    Point *u = q.front();
                    q.pop();
                    const auto itAdj = adj.find(u);
                    if (itAdj == adj.end())
                        continue;
                    for (Point *v : itAdj->second)
                    {
                        if (comp.count(v) == 0u)
                        {
                            comp.insert(v);
                            q.push(v);
                        }
                    }
                }

                std::size_t E = 0;
                for (Point *u : comp)
                {
                    const auto itA = adj.find(u);
                    if (itA != adj.end())
                        E += itA->second.size();
                }
                E /= 2u;
                const std::size_t V = comp.size();
                if (E == 0u)
                {
                    for (Point *u : comp)
                        visitedGlobal.insert(u);
                    continue;
                }

                for (Point *u : comp)
                    visitedGlobal.insert(u);

                ++componentsWithEdges;
                if (!(E == V || E == V - 1))
                {
                    tags |= AppInvalidTag::NonManifoldConnectivity;
                    return;
                }
            }

            if (componentsWithEdges >= 2)
                tags |= AppInvalidTag::NonManifoldConnectivity;
        }();
    }
}

static void BuildPlaneBasis(const glm::dvec3 &nUnit, glm::dvec3 &uOut, glm::dvec3 &vOut) noexcept
{
    const glm::dvec3 axis = (std::abs(nUnit.x) <= 0.9) ? glm::dvec3(1.0, 0.0, 0.0) : glm::dvec3(0.0, 1.0, 0.0);
    uOut = glm::normalize(glm::cross(axis, nUnit));
    vOut = glm::normalize(glm::cross(nUnit, uOut));
}

[[nodiscard]] static glm::dvec2 ProjectPointToPlane2d(const glm::dvec3 &p, const glm::dvec3 &origin,
                                                      const glm::dvec3 &u, const glm::dvec3 &v) noexcept
{
    const glm::dvec3 r = p - origin;
    return {glm::dot(r, u), glm::dot(r, v)};
}

[[nodiscard]] static bool SegmentsIntersectProper2D(const glm::dvec2 &a, const glm::dvec2 &b,
                                                     const glm::dvec2 &c, const glm::dvec2 &d,
                                                     double crossEps) noexcept
{
    auto cross = [](const glm::dvec2 &p, const glm::dvec2 &q) noexcept
    {
        return p.x * q.y - p.y * q.x;
    };
    const glm::dvec2 ab = b - a;
    const glm::dvec2 cd = d - c;
    const double t1 = cross(ab, c - a);
    const double t2 = cross(ab, d - a);
    const double t3 = cross(cd, a - c);
    const double t4 = cross(cd, b - c);
    auto sgn = [crossEps](double x) noexcept -> int
    {
        if (x > crossEps)
            return 1;
        if (x < -crossEps)
            return -1;
        return 0;
    };
    return sgn(t1) * sgn(t2) < 0 && sgn(t3) * sgn(t4) < 0;
}

[[nodiscard]] static bool Ring2dHasProperSelfIntersection(const std::vector<glm::dvec2> &ring,
                                                          double crossEps,
                                                          double minSegLen2) noexcept
{
    const std::size_t n = ring.size();
    if (n < 4)
        return false;
    for (std::size_t i = 0; i < n; ++i)
    {
        const std::size_t i1 = (i + 1) % n;
        if (glm::dot(ring[i1] - ring[i], ring[i1] - ring[i]) < minSegLen2)
            continue;
        for (std::size_t j = i + 1; j < n; ++j)
        {
            const std::size_t j1 = (j + 1) % n;
            if (i == j || i == j1 || i1 == j || i1 == j1)
                continue;
            if (glm::dot(ring[j1] - ring[j], ring[j1] - ring[j]) < minSegLen2)
                continue;
            if (SegmentsIntersectProper2D(ring[i], ring[i1], ring[j], ring[j1], crossEps))
                return true;
        }
    }
    return false;
}

[[nodiscard]] static bool TwoRingsHaveCrossingSegments2d(const std::vector<glm::dvec2> &ra,
                                                         const std::vector<glm::dvec2> &rb,
                                                         double crossEps,
                                                         double minSegLen2) noexcept
{
    const std::size_t na = ra.size();
    const std::size_t nb = rb.size();
    if (na < 2 || nb < 2)
        return false;
    for (std::size_t i = 0; i < na; ++i)
    {
        const std::size_t i1 = (i + 1) % na;
        if (glm::dot(ra[i1] - ra[i], ra[i1] - ra[i]) < minSegLen2)
            continue;
        for (std::size_t j = 0; j < nb; ++j)
        {
            const std::size_t j1 = (j + 1) % nb;
            if (glm::dot(rb[j1] - rb[j], rb[j1] - rb[j]) < minSegLen2)
                continue;
            if (SegmentsIntersectProper2D(ra[i], ra[i1], rb[j], rb[j1], crossEps))
                return true;
        }
    }
    return false;
}

void EvaluatePlanarLoopSelfIntersectionTags(const Solid &solid, AppInvalidTag &tags) noexcept
{
    for (Face *face : solid.faces)
    {
        if (face == nullptr || face->surface == nullptr || face->loops.empty())
            continue;
        if (!face->surface->IsPlanar())
            continue;

        const auto *const planar = static_cast<const PlanarSurface *>(face->surface.get());
        const glm::dvec3 n = planar->data.normal;
        const double nLen = glm::length(n);
        if (nLen < 1e-30)
            continue;
        const glm::dvec3 nHat = n / nLen;

        std::vector<std::vector<glm::dvec3>> rings3d;
        rings3d.reserve(face->loops.size());
        double maxSegLen = 0.0;
        for (const auto &loop : face->loops)
        {
            if (loop.size() < 3)
                continue;
            std::vector<glm::dvec3> ring;
            ring.reserve(loop.size());
            bool bad = false;
            for (const OrientedEdge &oe : loop)
            {
                if (oe.edge == nullptr || oe.GetStart() == nullptr)
                {
                    bad = true;
                    break;
                }
                ring.push_back(oe.GetStartPosition());
            }
            if (bad || ring.size() < 3)
                continue;
            const std::size_t rn = ring.size();
            for (std::size_t i = 0; i < rn; ++i)
            {
                const std::size_t i1 = (i + 1) % rn;
                maxSegLen = std::max(maxSegLen, glm::distance(ring[i], ring[i1]));
            }
            rings3d.push_back(std::move(ring));
        }
        if (rings3d.empty())
            continue;

        const glm::dvec3 origin = rings3d[0][0];
        glm::dvec3 uAxis{};
        glm::dvec3 vAxis{};
        BuildPlaneBasis(nHat, uAxis, vAxis);

        const double crossEps = std::max(1e-18, std::max(maxSegLen, 1.0) * 1e-14);
        const double minSegLen2 = std::max(1e-30, crossEps * crossEps * 1.0e6);

        std::vector<std::vector<glm::dvec2>> rings2d;
        rings2d.reserve(rings3d.size());
        for (const std::vector<glm::dvec3> &r3 : rings3d)
        {
            std::vector<glm::dvec2> r2;
            r2.reserve(r3.size());
            for (const glm::dvec3 &p : r3)
                r2.push_back(ProjectPointToPlane2d(p, origin, uAxis, vAxis));
            rings2d.push_back(std::move(r2));
        }

        for (const std::vector<glm::dvec2> &r2 : rings2d)
        {
            if (Ring2dHasProperSelfIntersection(r2, crossEps, minSegLen2))
            {
                tags |= AppInvalidTag::SelfIntersection;
                goto next_face;
            }
        }
        for (std::size_t a = 0; a < rings2d.size(); ++a)
        {
            for (std::size_t b = a + 1; b < rings2d.size(); ++b)
            {
                if (TwoRingsHaveCrossingSegments2d(rings2d[a], rings2d[b], crossEps, minSegLen2))
                {
                    tags |= AppInvalidTag::SelfIntersection;
                    goto next_face;
                }
            }
        }
    next_face:;
    }
}

} // namespace

AppInvalidTag EvaluateAppInvalidTagsForSolid(const Solid &solid) noexcept
{
    AppInvalidTag tags = AppInvalidTag::None;

    glm::dvec3 mn(std::numeric_limits<double>::max());
    glm::dvec3 mx(-std::numeric_limits<double>::max());
    bool anyPoint = false;

    auto considerPoint = [&](const glm::dvec3 &p)
    {
        anyPoint = true;
        mn = glm::min(mn, p);
        mx = glm::max(mx, p);
    };

    std::unordered_map<Edge *, std::vector<std::tuple<Face *, Point *, Point *>>> edgeUses;
    edgeUses.reserve(solid.faces.size() * 3u + 8u);
    std::vector<std::vector<glm::dvec3>> ringsForDegeneracy;
    ringsForDegeneracy.reserve(solid.faces.size());

    if (solid.faces.empty())
        tags |= AppInvalidTag::NullOrEmptyTopology;

    for (Face *face : solid.faces)
    {
        if (face == nullptr)
        {
            tags |= AppInvalidTag::NullOrEmptyTopology;
            continue;
        }
        if (face->surface == nullptr)
        {
            tags |= AppInvalidTag::NullOrEmptyTopology;
            continue;
        }
        if (face->loops.empty())
        {
            tags |= AppInvalidTag::NullOrEmptyTopology;
            continue;
        }

        for (const auto &loop : face->loops)
        {
            if (loop.empty())
            {
                tags |= AppInvalidTag::NullOrEmptyTopology;
                continue;
            }

            std::vector<glm::dvec3> ring;
            ring.reserve(loop.size());
            std::vector<std::pair<Edge *, std::pair<Point *, Point *>>> loopHalfEdges;
            loopHalfEdges.reserve(loop.size());
            bool loopTopologyBad = false;
            for (const OrientedEdge &oe : loop)
            {
                if (oe.edge == nullptr || oe.GetStart() == nullptr || oe.GetEnd() == nullptr)
                {
                    tags |= AppInvalidTag::NullOrEmptyTopology;
                    loopTopologyBad = true;
                    break;
                }
                loopHalfEdges.emplace_back(oe.edge, std::pair<Point *, Point *>(oe.GetStart(), oe.GetEnd()));
                ring.push_back(oe.GetStartPosition());
                considerPoint(ring.back());
            }
            if (loopTopologyBad)
                continue;

            for (const auto &he : loopHalfEdges)
                edgeUses[he.first].emplace_back(face, he.second.first, he.second.second);

            const std::size_t n = ring.size();
            if (n < 3)
            {
                tags |= AppInvalidTag::NullOrEmptyTopology;
                continue;
            }

            ringsForDegeneracy.push_back(std::move(ring));
        }
    }

    if (anyPoint && !ringsForDegeneracy.empty())
    {
        const double extent = BboxMaxExtent(mn, mx);
        const double scale = std::max(extent, 1.0);
        const double minArea2 = std::max(1e-36, (1e-24 * scale) * (1e-24 * scale));

        for (const std::vector<glm::dvec3> &r : ringsForDegeneracy)
        {
            const std::size_t n = r.size();
            if (n == 3)
            {
                if (TriangleDoubledAreaSquared(r[0], r[1], r[2]) < minArea2)
                {
                    tags |= AppInvalidTag::DegenerateTriangle;
                    break;
                }
            }
            else
            {
                bool bad = false;
                for (std::size_t i = 1; i + 1 < n; ++i)
                {
                    if (TriangleDoubledAreaSquared(r[0], r[i], r[i + 1]) < minArea2)
                    {
                        bad = true;
                        break;
                    }
                }
                if (bad)
                {
                    tags |= AppInvalidTag::DegenerateTriangle;
                    break;
                }
            }
        }
    }

    EvaluateEdgeConnectivityTags(solid, edgeUses, tags, nullptr);
    EvaluateVertexLinkConnectivityTags(solid, tags);
    EvaluatePlanarLoopSelfIntersectionTags(solid, tags);

    return tags;
}

void CollectOpenBoundaryEdgesForSolid(const Solid &solid, std::vector<const Edge *> &out) noexcept
{
    out.clear();
    std::unordered_map<Edge *, std::vector<std::tuple<Face *, Point *, Point *>>> edgeUses;
    edgeUses.reserve(solid.faces.size() * 3u + 8u);

    for (Face *face : solid.faces)
    {
        if (face == nullptr || face->surface == nullptr || face->loops.empty())
            continue;
        for (const auto &loop : face->loops)
        {
            if (loop.empty())
                continue;
            for (const OrientedEdge &oe : loop)
            {
                if (oe.edge == nullptr || oe.GetStart() == nullptr || oe.GetEnd() == nullptr)
                    continue;
                edgeUses[oe.edge].emplace_back(face, oe.GetStart(), oe.GetEnd());
            }
        }
    }

    AppInvalidTag discard = AppInvalidTag::None;
    std::vector<const Edge *> raw;
    raw.reserve(edgeUses.size() * 2u + 8u);
    EvaluateEdgeConnectivityTags(solid, edgeUses, discard, &raw);

    std::unordered_set<const Edge *> seen;
    seen.reserve(raw.size() * 2u + 8u);
    for (const Edge *e : raw)
    {
        if (e != nullptr && seen.insert(e).second)
            out.push_back(e);
    }
}

std::string DescribeAppInvalidTagsForLog(AppInvalidTag flags)
{
    if (!Any(flags))
        return {};

    std::string out;
    auto append = [&out](const char *name)
    {
        if (!out.empty())
            out += ", ";
        out += name;
    };

    using U = std::underlying_type_t<AppInvalidTag>;
    const U raw = static_cast<U>(flags);
    if (raw & static_cast<U>(AppInvalidTag::DegenerateTriangle))
        append("DegenerateTriangle");
    if (raw & static_cast<U>(AppInvalidTag::NullOrEmptyTopology))
        append("NullOrEmptyTopology");
    if (raw & static_cast<U>(AppInvalidTag::SelfIntersection))
        append("SelfIntersection");
    if (raw & static_cast<U>(AppInvalidTag::OpenBoundary))
        append("OpenBoundary");
    if (raw & static_cast<U>(AppInvalidTag::NonManifoldConnectivity))
        append("NonManifoldConnectivity");
    if (raw & static_cast<U>(AppInvalidTag::InconsistentFaceOrientation))
        append("InconsistentFaceOrientation");

    return out;
}

void RefreshSolidAppGeometryValidityCache(Solid &solid) noexcept
{
    solid.cachedAppInvalidGeometryTags = EvaluateAppInvalidTagsForSolid(solid);
    solid.cachedAppInvalidGeometryTagsFresh = true;
}

void InvalidateSolidAppGeometryValidityCache(Solid &solid) noexcept
{
    solid.cachedAppInvalidGeometryTags = AppInvalidTag::None;
    solid.cachedAppInvalidGeometryTagsFresh = false;
}

bool TryRepairInconsistentFaceOrientationSolid(Solid &solid) noexcept
{
    constexpr int kMaxPasses = 128;
    bool anyChange = false;
    for (int pass = 0; pass < kMaxPasses; ++pass)
    {
        bool passChange = false;
        std::unordered_map<Edge *, std::vector<std::tuple<Face *, Point *, Point *>>> uses;
        uses.reserve(solid.faces.size() * 3u + 8u);

        for (Face *f : solid.faces)
        {
            if (f == nullptr || f->dependency != &solid)
                continue;
            for (const auto &loop : f->loops)
            {
                for (const OrientedEdge &oe : loop)
                {
                    if (oe.edge == nullptr || oe.GetStart() == nullptr || oe.GetEnd() == nullptr)
                        continue;
                    uses[oe.edge].push_back({f, oe.GetStart(), oe.GetEnd()});
                }
            }
        }

        for (const auto &kv : uses)
        {
            Edge *const e = kv.first;
            const auto &vec = kv.second;
            if (e == nullptr || vec.size() != 2)
                continue;
            if (e->dependencies.size() != 2)
                continue;

            Face *const f0 = std::get<0>(vec[0]);
            Face *const f1 = std::get<0>(vec[1]);
            Point *const a0 = std::get<1>(vec[0]);
            Point *const b0 = std::get<2>(vec[0]);
            Point *const a1 = std::get<1>(vec[1]);
            Point *const b1 = std::get<2>(vec[1]);
            if (f0 == nullptr || f1 == nullptr || f0 == f1)
                continue;
            if (!(a0 == a1 && b0 == b1))
                continue;

            const bool p0 = f0->GetSurface().IsPlanar();
            const bool p1 = f1->GetSurface().IsPlanar();
            if (!p0 && !p1)
                continue;

            Face *flip = nullptr;
            if (p0 && p1)
                flip = (reinterpret_cast<std::uintptr_t>(f0) >= reinterpret_cast<std::uintptr_t>(f1)) ? f0 : f1;
            else
                flip = p0 ? f0 : f1;

            if (flip->FlipWindingIfPlanar())
            {
                passChange = true;
                anyChange = true;
            }
        }

        if (!passChange)
            break;
    }

    if (anyChange)
        LOG_BACK("Orientation repair: adjusted planar face winding for same-directed manifold edges");

    return anyChange;
}

bool TryRepairDegenerateSolidBRep(Solid &solid, DegenerateRepairStats *statsOut) noexcept
{
    DegenerateRepairStats local{};
    DegenerateRepairStats &s = statsOut != nullptr ? *statsOut : local;

    bool anyChange = false;
    std::unordered_set<Edge *> edges;
    CollectSolidEdges(solid, edges);
    if (edges.empty())
        return false;

    std::unordered_set<Point *> constrained;
    MarkConstrainedEndpoints(edges, constrained);

    const double weldEps = WeldEpsilonFromSolid(solid);
    if (weldEps > 0.0)
    {
        std::vector<Point *> pts;
        pts.reserve(edges.size() * 2u);
        std::unordered_map<Point *, int> idx;
        idx.reserve(edges.size() * 2u);

        for (Edge *e : edges)
        {
            if (e == nullptr || e->curve != nullptr || !e->bridgePoints.empty())
                continue;
            for (Point *p : {e->startPoint, e->endPoint})
            {
                if (p == nullptr)
                    continue;
                if (idx.find(p) != idx.end())
                    continue;
                idx.emplace(p, static_cast<int>(pts.size()));
                pts.push_back(p);
            }
        }

        if (pts.size() >= 2u)
        {
            Dsu dsu(static_cast<int>(pts.size()));
            std::unordered_map<WeldGridKey, std::vector<int>, WeldGridKeyHash> cells;
            cells.reserve(pts.size());

            for (int i = 0; i < static_cast<int>(pts.size()); ++i)
            {
                const WeldGridKey key = MakeWeldKey(pts[static_cast<std::size_t>(i)]->position, weldEps);
                cells[key].push_back(i);
            }

            auto tryUnite = [&](int i, int j)
            {
                if (i == j)
                    return;
                Point *const a = pts[static_cast<std::size_t>(i)];
                Point *const b = pts[static_cast<std::size_t>(j)];
                if (constrained.find(a) != constrained.end() || constrained.find(b) != constrained.end())
                    return;
                const glm::dvec3 d = b->position - a->position;
                const double d2 = glm::dot(d, d);
                if (d2 > weldEps * weldEps)
                    return;
                const int ri = dsu.find(i);
                const int rj = dsu.find(j);
                if (ri == rj)
                    return;
                dsu.unite(i, j);
                ++s.weldUnionPairs;
            };

            for (int i = 0; i < static_cast<int>(pts.size()); ++i)
            {
                const WeldGridKey base = MakeWeldKey(pts[static_cast<std::size_t>(i)]->position, weldEps);
                for (int dx = -1; dx <= 1; ++dx)
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dz = -1; dz <= 1; ++dz)
                        {
                            const WeldGridKey nk{base.x + dx, base.y + dy, base.z + dz};
                            const auto it = cells.find(nk);
                            if (it == cells.end())
                                continue;
                            for (const int j : it->second)
                            {
                                if (j <= i)
                                    continue;
                                tryUnite(i, j);
                            }
                        }
            }

            std::unordered_map<int, Point *> rootCanon;
            rootCanon.reserve(pts.size());
            for (int i = 0; i < static_cast<int>(pts.size()); ++i)
            {
                const int r = dsu.find(i);
                Point *const p = pts[static_cast<std::size_t>(i)];
                const auto it = rootCanon.find(r);
                if (it == rootCanon.end() ||
                    reinterpret_cast<std::uintptr_t>(p) < reinterpret_cast<std::uintptr_t>(it->second))
                    rootCanon[r] = p;
            }

            std::unordered_map<Point *, Point *> rep;
            rep.reserve(pts.size() * 2u);
            for (int i = 0; i < static_cast<int>(pts.size()); ++i)
            {
                const int r = dsu.find(i);
                rep[pts[static_cast<std::size_t>(i)]] = rootCanon[r];
            }

            for (Edge *e : edges)
            {
                if (e == nullptr || e->curve != nullptr || !e->bridgePoints.empty())
                    continue;
                Point *ns = e->startPoint;
                Point *ne = e->endPoint;
                if (ns != nullptr)
                {
                    const auto it = rep.find(ns);
                    if (it != rep.end())
                        ns = it->second;
                }
                if (ne != nullptr)
                {
                    const auto it = rep.find(ne);
                    if (it != rep.end())
                        ne = it->second;
                }
                if (ns == e->startPoint && ne == e->endPoint)
                    continue;
                if (e->startPoint != nullptr)
                    e->startPoint->dependencies.erase(e);
                if (e->endPoint != nullptr)
                    e->endPoint->dependencies.erase(e);
                e->startPoint = ns;
                e->endPoint = ne;
                if (e->startPoint != nullptr)
                    e->startPoint->dependencies.insert(e);
                if (e->endPoint != nullptr)
                    e->endPoint->dependencies.insert(e);
                ++s.edgesRetargeted;
                anyChange = true;
            }
        }
    }

    const double minA2 = MinFanTriangleAreaSquaredThreshold(solid);
    const std::vector<Face *> snapshot(solid.faces.begin(), solid.faces.end());
    for (Face *f : snapshot)
    {
        if (f == nullptr)
            continue;
        if (std::find(solid.faces.begin(), solid.faces.end(), f) == solid.faces.end())
            continue;
        if (f->dependency != &solid)
            continue;
        if (!FaceStillDegenerateByFanRule(f, minA2))
            continue;
        RemoveFaceFromSolid(solid, f);
        ++s.facesRemoved;
        anyChange = true;
    }

    if (anyChange)
        LOG_BACK("Degenerate repair: weldPairs=", s.weldUnionPairs, " edgesRetargeted=", s.edgesRetargeted,
                 " facesRemoved=", s.facesRemoved);

    return anyChange;
}

static void RetargetHalfEdgeForDuplicateMerge(OrientedEdge &oe, Face *f, Edge *dup, Edge *keeper) noexcept
{
    if (dup == nullptr || keeper == nullptr || f == nullptr)
        return;
    Point *const logicalS = oe.GetStart();
    Point *const logicalE = oe.GetEnd();
    dup->dependencies.erase(f);
    oe.edge = keeper;
    if (logicalS == keeper->startPoint && logicalE == keeper->endPoint)
        oe.reversed = false;
    else if (logicalS == keeper->endPoint && logicalE == keeper->startPoint)
        oe.reversed = true;
    else
    {
        LOG_WARN("Duplicate edge merge: could not match logical walk to keeper endpoints");
        oe.reversed = false;
    }
    keeper->dependencies.insert(f);
}

bool TryMergeDuplicateStraightEdgesSolid(Solid &solid) noexcept
{
    std::unordered_set<Edge *> edgeSet;
    edgeSet.reserve(solid.faces.size() * 3u + 8u);
    for (Face *f : solid.faces)
    {
        if (f == nullptr || f->dependency != &solid)
            continue;
        for (const auto &loop : f->loops)
        {
            for (const OrientedEdge &oe : loop)
            {
                if (oe.edge != nullptr)
                    edgeSet.insert(oe.edge);
            }
        }
    }

    struct EndpointKey
    {
        Point *lo = nullptr;
        Point *hi = nullptr;
        bool operator==(const EndpointKey &o) const noexcept { return lo == o.lo && hi == o.hi; }
    };
    struct EndpointKeyHash
    {
        std::size_t operator()(const EndpointKey &k) const noexcept
        {
            return std::hash<Point *>{}(k.lo) ^ (std::hash<Point *>{}(k.hi) << 1);
        }
    };

    auto makeKey = [](Edge *e) -> EndpointKey
    {
        if (e == nullptr || e->startPoint == nullptr || e->endPoint == nullptr)
            return {};
        Point *const a = e->startPoint;
        Point *const b = e->endPoint;
        if (a == b)
            return {};
        if (reinterpret_cast<std::uintptr_t>(a) <= reinterpret_cast<std::uintptr_t>(b))
            return {a, b};
        return {b, a};
    };

    std::unordered_map<EndpointKey, std::vector<Edge *>, EndpointKeyHash> buckets;
    buckets.reserve(edgeSet.size() * 2u + 8u);
    for (Edge *e : edgeSet)
    {
        if (e == nullptr || e->curve != nullptr || !e->bridgePoints.empty())
            continue;
        const EndpointKey k = makeKey(e);
        if (k.lo == nullptr)
            continue;
        buckets[k].push_back(e);
    }

    bool anyChange = false;
    std::size_t mergedCount = 0;

    for (auto &kv : buckets)
    {
        std::vector<Edge *> &vec = kv.second;
        if (vec.size() < 2u)
            continue;
        std::sort(vec.begin(), vec.end(), [](Edge *a, Edge *b) noexcept
        {
            return reinterpret_cast<std::uintptr_t>(a) < reinterpret_cast<std::uintptr_t>(b);
        });
        vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
        if (vec.size() < 2u)
            continue;

        Edge *const keeper = vec[0];
        for (std::size_t i = 1; i < vec.size(); ++i)
        {
            Edge *dup = vec[i];
            if (dup == nullptr || dup == keeper)
                continue;

            for (Face *f : solid.faces)
            {
                if (f == nullptr || f->dependency != &solid)
                    continue;
                for (auto &loop : f->loops)
                {
                    for (OrientedEdge &oe : loop)
                    {
                        if (oe.edge == dup)
                            RetargetHalfEdgeForDuplicateMerge(oe, f, dup, keeper);
                    }
                }
            }

            if (dup->startPoint != nullptr)
                dup->startPoint->dependencies.erase(dup);
            if (dup->endPoint != nullptr)
                dup->endPoint->dependencies.erase(dup);
            dup->startPoint = nullptr;
            dup->endPoint = nullptr;
            dup->dependencies.clear();
            ++mergedCount;
            anyChange = true;
        }
    }

    if (anyChange)
        LOG_BACK("Duplicate edge merge: orphaned ", mergedCount, " redundant straight Edge records");

    return anyChange;
}

} // namespace GeometryValidity
