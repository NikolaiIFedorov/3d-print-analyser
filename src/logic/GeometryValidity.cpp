#include "GeometryValidity.hpp"

#include "Geometry/Edge.hpp"
#include "Geometry/Face.hpp"
#include "Geometry/OrientedEdge.hpp"
#include "Geometry/Point.hpp"
#include "Geometry/Solid.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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

    std::unordered_map<Edge *, std::vector<std::pair<Point *, Point *>>> edgeUses;
    edgeUses.reserve(solid.faces.size() * 3u + 8u);
    std::vector<std::vector<glm::dvec3>> ringsForDegeneracy;
    ringsForDegeneracy.reserve(solid.faces.size());

    if (solid.faces.empty())
        tags |= AppInvalidTag::NullOrEmptyTopology;

    for (const Face *face : solid.faces)
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
                edgeUses[he.first].push_back(he.second);

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

    for (const auto &kv : edgeUses)
    {
        const Edge *const e = kv.first;
        const auto &vec = kv.second;
        (void)e;
        const int c = static_cast<int>(vec.size());
        if (c == 1)
            tags |= AppInvalidTag::OpenBoundary;
        else if (c > 2)
            tags |= AppInvalidTag::NonManifoldConnectivity;
        else if (c == 2)
        {
            const Point *a0 = vec[0].first;
            const Point *b0 = vec[0].second;
            const Point *a1 = vec[1].first;
            const Point *b1 = vec[1].second;
            const bool opposite = (a0 == b1 && b0 == a1);
            const bool same = (a0 == a1 && b0 == b1);
            if (same)
                tags |= AppInvalidTag::InconsistentFaceOrientation;
            else if (!opposite)
                tags |= AppInvalidTag::NonManifoldConnectivity;
        }
    }

    return tags;
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

} // namespace GeometryValidity
