#include "Structure/StructureTriangulation.hpp"

#include "Geometry/Face.hpp"
#include "Geometry/Surface.hpp"

#include <unordered_map>

// Phase B2a (see documentation/implementations/structure_face_triangulation_2026-05-11.md):
// CMake flag rename, new module skeleton, slider state, stub bake. The algorithm (CGAL straight
// skeleton inset + strip + fillet) lands in B2b–B2e; B2f wires the cache invalidation hooks.

namespace StructureTriangulation
{
namespace
{

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
    // B2a stub: cache the lookup machinery and return empty. B2b replaces this body with the
    // B-rep -> 2D polygon construction (polyline + project), B2c adds the CGAL straight-skeleton
    // inset, B2d/e add strip + fillet, B2f wires invalidation hooks into Display's slider drag.
    if (face == nullptr)
        return {};
    const CacheKey key{face, params.insetMm, params.chordTolMm, params.minFeatureMm};
    auto &cache = BakeCache();
    if (auto it = cache.find(key); it != cache.end())
        return it->second;
    SegmentList segments; // empty until B2b
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
