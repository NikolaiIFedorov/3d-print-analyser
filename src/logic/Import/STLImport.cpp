#include "STLImport.hpp"
#include "GeometryExperiments.hpp"
#include "utils/log.hpp"

#if defined(CAD_CGAL_PLANAR_REMESH_EXPERIMENT_ENABLED)
#include "STLCgalPlanarExperiment.hpp"
#endif

#include <fstream>
#include <array>
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

struct WeldGridKey
{
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;

    bool operator==(const WeldGridKey &other) const
    {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct WeldGridKeyHash
{
    std::size_t operator()(const WeldGridKey &key) const
    {
        std::size_t h = std::hash<std::int64_t>{}(key.x);
        h ^= std::hash<std::int64_t>{}(key.y) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<std::int64_t>{}(key.z) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

struct SpatialPointWeld
{
    double tolerance = 0.0;
    std::unordered_map<WeldGridKey, std::vector<Point *>, WeldGridKeyHash> cells;
    std::size_t pointCount = 0;
};

static void ReportLoopProgress(
    const ImportProgressCallback *progress,
    const char *phase,
    std::uint64_t index,
    std::uint64_t total,
    float rangeStart01,
    float rangeEnd01)
{
    if (progress == nullptr || !*progress || total == 0)
        return;

    const std::uint64_t stride = std::max<std::uint64_t>(1, total / 200);
    if ((index + 1) != total && (index % stride) != 0)
        return;

    const float localProgress01 = static_cast<float>(index + 1) / static_cast<float>(total);
    ReportImportProgress(progress, phase, MapImportProgress(localProgress01, rangeStart01, rangeEnd01));
}

/// Binary STL stores float32 coordinates, so weld only within a few float ULPs at file scale.
static double BinaryStlWeldToleranceFromBounds(const glm::dvec3 &boundMin, const glm::dvec3 &boundMax)
{
    const double maxAbsCoord = std::max({
        std::abs(boundMin.x), std::abs(boundMin.y), std::abs(boundMin.z),
        std::abs(boundMax.x), std::abs(boundMax.y), std::abs(boundMax.z),
        1.0});
    return 2.0 * static_cast<double>(std::numeric_limits<float>::epsilon()) * maxAbsCoord;
}

static std::int64_t WeldCellIndex(double coord, double tolerance)
{
    return static_cast<std::int64_t>(std::floor(coord / tolerance));
}

static WeldGridKey MakeWeldGridKey(const glm::dvec3 &pos, double tolerance)
{
    return {
        WeldCellIndex(pos.x, tolerance),
        WeldCellIndex(pos.y, tolerance),
        WeldCellIndex(pos.z, tolerance)};
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

#if defined(CAD_CGAL_PLANAR_REMESH_EXPERIMENT_ENABLED)
    if (GeometryExperiments::kUseCgalRemeshPlanarPatchesForStl &&
        STLCgalPlanarExperiment::TryRemeshPlanarPatchesReplacingSolidFaces(scene, solid, diagOut))
    {
        // CGAL still emits a triangle mesh per planar patch (CDT); scene merge removes internal
        // coplanar edges so wireframe / patches match large facets.
        scene->MergeCoplanarFaces(solid, diagOut, &mergeProgress);
        return;
    }
#endif

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

static Point *GetOrCreateNearbyFloatPoint(Scene *scene, SpatialPointWeld &weld, const glm::dvec3 &pos)
{
    const WeldGridKey key = MakeWeldGridKey(pos, weld.tolerance);
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dz = -1; dz <= 1; ++dz)
            {
                const WeldGridKey neighborKey{key.x + dx, key.y + dy, key.z + dz};
                auto cellIt = weld.cells.find(neighborKey);
                if (cellIt == weld.cells.end())
                    continue;

                for (Point *candidate : cellIt->second)
                {
                    const glm::dvec3 delta = candidate->position - pos;
                    if (std::abs(delta.x) <= weld.tolerance &&
                        std::abs(delta.y) <= weld.tolerance &&
                        std::abs(delta.z) <= weld.tolerance)
                    {
                        return candidate;
                    }
                }
            }

    Point *p = scene->CreatePoint(pos);
    weld.cells[key].push_back(p);
    ++weld.pointCount;
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
    SpatialPointWeld pointWeld;
    std::vector<Face *> faces;
    faces.reserve(triangleCount);

    const std::streampos triBlockStart = file.tellg();
    glm::dvec3 boundMin(std::numeric_limits<double>::max());
    glm::dvec3 boundMax(std::numeric_limits<double>::lowest());
    for (uint32_t i = 0; i < triangleCount; ++i)
    {
        float data[12];
        file.read(reinterpret_cast<char *>(data), 48);
        uint16_t attr;
        file.read(reinterpret_cast<char *>(&attr), 2);
        if (!file)
            return LOG_FALSE("Failed reading triangle " + std::to_string(i) + " (bounds pass)");

        for (int v = 0; v < 3; ++v)
        {
            glm::dvec3 pos(data[3 + v * 3], data[4 + v * 3], data[5 + v * 3]);
            boundMin = glm::min(boundMin, pos);
            boundMax = glm::max(boundMax, pos);
        }
        ReportLoopProgress(progress, "Scanning STL bounds...", i, triangleCount, 0.02f, 0.15f);
    }
    pointWeld.tolerance = BinaryStlWeldToleranceFromBounds(boundMin, boundMax);
    file.clear();
    file.seekg(triBlockStart);

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
            glm::dvec3 pos(data[3 + v * 3], data[4 + v * 3], data[5 + v * 3]);
            pts[v] = GetOrCreateNearbyFloatPoint(scene, pointWeld, pos);
        }

        if (pts[0] == pts[1] || pts[1] == pts[2] || pts[0] == pts[2])
            continue;

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
        ReportLoopProgress(progress, "Reading STL triangles...", i, triangleCount, 0.15f, 0.70f);
    }

    double parseMs = std::chrono::duration<double, std::milli>(Clock::now() - tStart).count();
    double mergeMs = 0.0;
    if (!faces.empty())
    {
        ReportImportProgress(progress, "Merging coplanar STL faces...", 0.70f);
        const Clock::time_point tMergeStart = Clock::now();
        Solid *solid = scene->CreateSolid(faces);
        MergeStlCoplanarMaybe(scene, solid, stats, progress);
        mergeMs = std::chrono::duration<double, std::milli>(Clock::now() - tMergeStart).count();
    }

    if (stats != nullptr)
    {
        stats->isBinary = true;
        stats->triangleCount = triangleCount;
        stats->uniquePoints = pointWeld.pointCount;
        stats->faces = faces.size();
        stats->parseMs = parseMs;
        stats->mergeMs = mergeMs;
        stats->totalMs = parseMs + mergeMs;
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
            continue;

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
        Solid *solid = scene->CreateSolid(faces);
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
