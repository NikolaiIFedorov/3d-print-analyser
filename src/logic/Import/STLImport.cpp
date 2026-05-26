#include "STLImport.hpp"
#include "GeometryExperiments.hpp"
#include "GeometryValidity.hpp"
#include "utils/log.hpp"

#include <fstream>
#include <array>
#include <bit>
#include <map>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

struct Vec3Compare
{
    bool operator()(const glm::dvec3 &a, const glm::dvec3 &b) const
    {
        if (a.x != b.x)
            return a.x < b.x;
        if (a.y != b.y)
            return a.y < b.y;
        return a.z < b.z;
    }
};

static glm::dvec3 QuantizePosition(const glm::dvec3 &p, double weldEps)
{
    if (!(weldEps > 0.0) || !std::isfinite(weldEps))
        return p;
    return glm::dvec3(
        std::round(p.x / weldEps) * weldEps,
        std::round(p.y / weldEps) * weldEps,
        std::round(p.z / weldEps) * weldEps);
}

struct BinaryFloatKey
{
    std::uint32_t xBits = 0;
    std::uint32_t yBits = 0;
    std::uint32_t zBits = 0;

    bool operator==(const BinaryFloatKey &other) const
    {
        return xBits == other.xBits && yBits == other.yBits && zBits == other.zBits;
    }
};

struct BinaryFloatKeyHash
{
    std::size_t operator()(const BinaryFloatKey &key) const
    {
        std::size_t h = std::hash<std::uint32_t>{}(key.xBits);
        h ^= std::hash<std::uint32_t>{}(key.yBits) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<std::uint32_t>{}(key.zBits) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

static std::uint32_t FloatBits(float v)
{
    return std::bit_cast<std::uint32_t>(v);
}

static Point *GetOrCreateExactBinaryFloatPoint(
    Scene *scene,
    std::unordered_map<BinaryFloatKey, Point *, BinaryFloatKeyHash> &pointMap,
    float x,
    float y,
    float z)
{
    const BinaryFloatKey key{FloatBits(x), FloatBits(y), FloatBits(z)};
    const auto it = pointMap.find(key);
    if (it != pointMap.end())
        return it->second;
    Point *const p = scene->CreatePoint(glm::dvec3(static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)));
    pointMap.emplace(key, p);
    return p;
}

/// Runs coplanar face merge after STL triangle soup, or only captures topology when merge is disabled.
static void MergeStlCoplanarMaybe(
    Scene *scene,
    Solid *solid,
    STLImportStats *stats,
    const ImportProgressCallback *progress)
{
    MergeCoplanarDiagnostics *diagOut = stats ? &stats->mergeDiagnostics : nullptr;
    if (stats)
        stats->hasMergeDiagnostics = true;

    SceneProgressCallback mergeProgress = [progress](float mergeProgress01)
    {
        ReportImportProgress(
            progress,
            "Merging coplanar STL faces...",
            MapImportProgress(mergeProgress01, 0.70f, 0.85f));
    };

    if (GeometryExperiments::kSkipStlMergeCoplanarFaces)
    {
        if (diagOut != nullptr)
            scene->CollectCoplanarMergeTopology(solid, diagOut);
        return;
    }

    // Detect raw self-intersection before merge. Coplanar merge may otherwise tear down
    // shared topology in intersecting regions and surface false open-boundary blame.
    if (solid != nullptr)
    {
        const GeometryValidity::AppInvalidTag rawTags = GeometryValidity::EvaluateAppInvalidTagsForSolid(*solid);
        if (GeometryValidity::Any(rawTags & GeometryValidity::AppInvalidTag::SelfIntersection))
        {
            LOG_DESC("STL merge skipped: pre-merge self-intersection detected; preserving raw import topology");
            solid->cachedAppInvalidGeometryTags = rawTags;
            solid->cachedAppInvalidGeometryTagsFresh = true;
            if (diagOut != nullptr)
                scene->CollectCoplanarMergeTopology(solid, diagOut);
            return;
        }
    }

    scene->MergeCoplanarFaces(solid, diagOut, &mergeProgress);
}

static bool IsBinarySTL(std::ifstream &file, uint32_t &triangleCount)
{
    file.seekg(0, std::ios::end);
    auto fileSize = file.tellg();
    file.seekg(0);

    if (fileSize < 84)
        return false;

    char header[80];
    file.read(header, 80);
    file.read(reinterpret_cast<char *>(&triangleCount), 4);

    auto expectedSize = 84 + static_cast<std::streamoff>(triangleCount) * 50;
    return fileSize == expectedSize;
}

static Point *GetOrCreatePoint(
    Scene *scene,
    std::map<glm::dvec3, Point *, Vec3Compare> &pointMap,
    const glm::dvec3 &pos,
    double weldEps)
{
    const glm::dvec3 key = QuantizePosition(pos, weldEps);
    auto it = pointMap.find(key);
    if (it != pointMap.end())
        return it->second;

    Point *p = scene->CreatePoint(key);
    pointMap[key] = p;
    return p;
}

static bool ImportBinary(
    std::ifstream &file,
    Scene *scene,
    uint32_t triangleCount,
    STLImportStats *stats,
    const ImportProgressCallback *progress)
{
    using Clock = std::chrono::steady_clock;
    const Clock::time_point tStart = Clock::now();
    std::unordered_map<BinaryFloatKey, Point *, BinaryFloatKeyHash> pointMap;
    pointMap.reserve(static_cast<std::size_t>(triangleCount) * 2u + 8u);
    std::vector<Face *> faces;
    faces.reserve(triangleCount);
    std::uint64_t skippedDegenerateTriangles = 0;

    for (uint32_t i = 0; i < triangleCount; ++i)
    {
        float data[12];
        file.read(reinterpret_cast<char *>(data), 48);

        uint16_t attr;
        file.read(reinterpret_cast<char *>(&attr), 2);

        if (!file)
            return LOG_FALSE("Failed reading triangle " + std::to_string(i));

        std::array<Point *, 3> pts;
        for (int v = 0; v < 3; ++v)
        {
            const float x = data[3 + v * 3];
            const float y = data[4 + v * 3];
            const float z = data[5 + v * 3];
            pts[v] = GetOrCreateExactBinaryFloatPoint(scene, pointMap, x, y, z);
        }

        if (pts[0] == pts[1] || pts[1] == pts[2] || pts[0] == pts[2])
        {
            ++skippedDegenerateTriangles;
            continue;
        }

        Edge *e1 = scene->CreateEdge(pts[0], pts[1]);
        Edge *e2 = scene->CreateEdge(pts[1], pts[2]);
        Edge *e3 = scene->CreateEdge(pts[2], pts[0]);

        Face *f = scene->CreateFace({{e1, e2, e3}});

        // Use the stored STL normal to ensure the face normal is outward-pointing.
        glm::dvec3 storedNormal(data[0], data[1], data[2]);
        if (glm::length(storedNormal) > 0.5)
        {
            auto *planar = dynamic_cast<PlanarSurface *>(f->surface.get());
            if (planar && glm::dot(planar->data.normal, storedNormal) < 0.0)
                planar->data.normal = -planar->data.normal;
        }

        faces.push_back(f);
        if (progress != nullptr && *progress && triangleCount > 0 && (i % std::max<std::uint32_t>(1, triangleCount / 200)) == 0)
        {
            const float local = static_cast<float>(i + 1) / static_cast<float>(triangleCount);
            ReportImportProgress(progress, "Reading STL triangles...", MapImportProgress(local, 0.02f, 0.70f));
        }
    }

    double parseMs = std::chrono::duration<double, std::milli>(Clock::now() - tStart).count();
    double mergeMs = 0.0;
    if (!faces.empty())
    {
        ReportImportProgress(progress, "Merging coplanar STL faces...", 0.70f);
        const Clock::time_point tMergeStart = Clock::now();
        // STL overlap/self-intersection cases are sensitive to generic topology repairs; keep
        // imported face graph untouched and rely on explicit repair actions in UI flow.
        Solid *solid = scene->CreateSolid(faces, false);
        MergeStlCoplanarMaybe(scene, solid, stats, progress);
        mergeMs = std::chrono::duration<double, std::milli>(Clock::now() - tMergeStart).count();
    }

    if (stats != nullptr)
    {
        stats->isBinary = true;
        stats->triangleCount = triangleCount;
        stats->uniquePoints = pointMap.size();
        stats->faces = faces.size();
        stats->parseMs = parseMs;
        stats->mergeMs = mergeMs;
        stats->totalMs = parseMs + mergeMs;
    }
    if (skippedDegenerateTriangles > 0)
    {
        LOG_WARN("STL import dropped", std::to_string(skippedDegenerateTriangles),
                 "degenerate triangles after weld");
    }
    return true;
}

static bool ImportASCII(
    std::ifstream &file,
    Scene *scene,
    STLImportStats *stats,
    const ImportProgressCallback *progress)
{
    using Clock = std::chrono::steady_clock;
    const Clock::time_point tStart = Clock::now();
    std::map<glm::dvec3, Point *, Vec3Compare> pointMap;
    std::vector<Face *> faces;
    const double weldEps = 1e-6; // ASCII path: no bbox pre-scan; conservative absolute snap
    std::uint64_t skippedDegenerateTriangles = 0;

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    file.seekg(0);

    std::string line;
    std::uint64_t faceLineIndex = 0;
    while (std::getline(file, line))
    {
        if (line.find("outer loop") == std::string::npos)
            continue;

        std::array<Point *, 3> pts;
        for (int v = 0; v < 3; ++v)
        {
            if (!std::getline(file, line))
                return LOG_FALSE("Unexpected end of ASCII STL");

            float x, y, z;
            if (std::sscanf(line.c_str(), " vertex %f %f %f", &x, &y, &z) != 3)
                return LOG_FALSE("Failed parsing vertex in ASCII STL");

            pts[v] = GetOrCreatePoint(scene, pointMap, glm::dvec3(x, y, z), weldEps);
        }

        if (pts[0] == pts[1] || pts[1] == pts[2] || pts[0] == pts[2])
        {
            ++skippedDegenerateTriangles;
            continue;
        }

        Edge *e1 = scene->CreateEdge(pts[0], pts[1]);
        Edge *e2 = scene->CreateEdge(pts[1], pts[2]);
        Edge *e3 = scene->CreateEdge(pts[2], pts[0]);

        Face *f = scene->CreateFace({{e1, e2, e3}});

        // ASCII STL spec mandates CCW vertex order viewed from outside.
        {
            glm::dvec3 expectedNormal = glm::normalize(
                glm::cross(pts[1]->position - pts[0]->position,
                           pts[2]->position - pts[0]->position));
            auto *planar = dynamic_cast<PlanarSurface *>(f->surface.get());
            if (planar && glm::dot(planar->data.normal, expectedNormal) < 0.0)
                planar->data.normal = -planar->data.normal;
        }

        faces.push_back(f);

        if (progress != nullptr && *progress && fileSize > 0 && (faceLineIndex++ % 256) == 0)
        {
            const std::streampos pos = file.tellg();
            if (pos != std::streampos(-1))
            {
                const std::streamoff posOffset = static_cast<std::streamoff>(pos);
                ReportImportProgress(
                    progress,
                    "Reading ASCII STL triangles...",
                    MapImportProgress(
                        std::clamp(static_cast<float>(static_cast<double>(posOffset) / static_cast<double>(fileSize)), 0.0f, 1.0f),
                        0.05f,
                        0.70f));
            }
        }
    }
    ReportImportProgress(progress, "Reading ASCII STL triangles...", 0.70f);

    double parseMs = std::chrono::duration<double, std::milli>(Clock::now() - tStart).count();
    double mergeMs = 0.0;
    if (!faces.empty())
    {
        ReportImportProgress(progress, "Merging coplanar STL faces...", 0.70f);
        const Clock::time_point tMergeStart = Clock::now();
        // STL overlap/self-intersection cases are sensitive to generic topology repairs; keep
        // imported face graph untouched and rely on explicit repair actions in UI flow.
        Solid *solid = scene->CreateSolid(faces, false);
        MergeStlCoplanarMaybe(scene, solid, stats, progress);
        mergeMs = std::chrono::duration<double, std::milli>(Clock::now() - tMergeStart).count();
    }

    if (stats != nullptr)
    {
        stats->isBinary = false;
        stats->triangleCount = 0;
        stats->uniquePoints = pointMap.size();
        stats->faces = faces.size();
        stats->parseMs = parseMs;
        stats->mergeMs = mergeMs;
        stats->totalMs = parseMs + mergeMs;
    }
    if (skippedDegenerateTriangles > 0)
    {
        LOG_WARN("ASCII STL import dropped", std::to_string(skippedDegenerateTriangles),
                 "degenerate triangles after weld");
    }
    return true;
}

bool STLImport::Import(
    const std::string &filePath,
    Scene *scene,
    STLImportStats *stats,
    const ImportProgressCallback *progress)
{
    ReportImportProgress(progress, "Opening STL file...", 0.0f);
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open())
        return LOG_FALSE("Failed to open STL file: " + filePath);

    uint32_t triangleCount = 0;
    if (IsBinarySTL(file, triangleCount))
    {
        LOG_DESC("Importing binary STL: " + filePath)
        return ImportBinary(file, scene, triangleCount, stats, progress);
    }

    file.clear();
    file.seekg(0);
    file.close();

    file.open(filePath);
    if (!file.is_open())
        return LOG_FALSE("Failed to reopen STL file as text: " + filePath);

    LOG_DESC("Importing ASCII STL: " + filePath)
    return ImportASCII(file, scene, stats, progress);
}
