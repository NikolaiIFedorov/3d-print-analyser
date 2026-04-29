#include "Stringing.hpp"
#include "logic/Analysis/utils/Slice.hpp"
#include "scene/Geometry/AllGeometry.hpp"

#include <numeric>
#include <unordered_set>

// ---------------------------------------------------------------------------
// Union-Find used to group segments into connected contour loops
// ---------------------------------------------------------------------------
namespace
{

    struct UnionFind
    {
        std::vector<int> parent;

        explicit UnionFind(int n) : parent(n)
        {
            std::iota(parent.begin(), parent.end(), 0);
        }

        int find(int x)
        {
            return parent[x] == x ? x : parent[x] = find(parent[x]);
        }

        void unite(int x, int y)
        {
            parent[find(x)] = find(y);
        }
    };

    static bool SharesEndpoint(const Segment &s1, const Segment &s2)
    {
        constexpr double eps = 1e-10;
        return glm::length(glm::dvec2(s1.a - s2.a)) < eps ||
               glm::length(glm::dvec2(s1.a - s2.b)) < eps ||
               glm::length(glm::dvec2(s1.b - s2.a)) < eps ||
               glm::length(glm::dvec2(s1.b - s2.b)) < eps;
    }

} // namespace

// ---------------------------------------------------------------------------

std::vector<FaceFlaw> Stringing::Analyze(const Solid *solid, std::optional<ZBounds> bounds,
                                         std::vector<BridgeSurface> *) const
{
    std::vector<FaceFlaw> results;

    auto [zMin, zMax] = bounds.value_or(Slice::GetZBounds(solid));
    if (zMax - zMin < layerHeight)
        return results;

    struct FaceInfo
    {
        ZBounds zBounds;
    };
    std::unordered_map<const Face *, FaceInfo> faceInfos;

    auto layers = Slice::Range(solid, zMin, zMax, layerHeight);

    for (const auto &layer : layers)
    {
        const auto &segs = layer.segments;
        int n = static_cast<int>(segs.size());
        if (n < 2)
            continue;

        double z = segs[0].a.z;

        // Group segments into connected components: two segments belong to the
        // same contour loop when they share an endpoint.
        UnionFind uf(n);
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j)
                if (SharesEndpoint(segs[i], segs[j]))
                    uf.unite(i, j);

        // Count distinct contour loops at this layer
        std::unordered_set<int> roots;
        roots.reserve(n);
        for (int i = 0; i < n; ++i)
            roots.insert(uf.find(i));

        if (static_cast<int>(roots.size()) <= maxContourCount)
            continue;

        // More loops than the threshold — every face at this layer is stringing-prone
        for (const auto &seg : segs)
        {
            if (!seg.face)
                continue;

            auto it = faceInfos.find(seg.face);
            if (it == faceInfos.end())
                faceInfos[seg.face] = {{z, z}};
            else
            {
                it->second.zBounds.zMin = std::min(it->second.zBounds.zMin, z);
                it->second.zBounds.zMax = std::max(it->second.zBounds.zMax, z);
            }
        }
    }

    results.reserve(faceInfos.size());
    for (const auto &[face, info] : faceInfos)
        results.push_back({face, FaceFlawKind::STRINGING, info.zBounds});

    return results;
}
