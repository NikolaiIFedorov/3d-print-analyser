#include "GeometryValidity.hpp"

#include "Geometry/Edge.hpp"
#include "Geometry/Face.hpp"
#include "Geometry/OrientedEdge.hpp"
#include "Geometry/Point.hpp"
#include "Geometry/Solid.hpp"

#include <cmath>
#include <limits>
#include <unordered_map>
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

} // namespace GeometryValidity
