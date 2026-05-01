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
#include <cmath>
#include <limits>

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

/// Weld vertices that differ only by floating-point noise so triangles share edges for coplanar merge.
static double StlWeldEpsilonFromDiagonal(double diagonal)
{
    if (!std::isfinite(diagonal) || diagonal <= 0.0)
        return 1e-6;
    return std::clamp(1e-7 * diagonal, 1e-10, 1e-3);
}

/// Runs coplanar face merge after STL triangle soup, or only captures topology when merge is disabled.
static void MergeStlCoplanarMaybe(Scene *scene, Solid *solid, STLImportStats *stats)
{
    MergeCoplanarDiagnostics *diagOut = stats ? &stats->mergeDiagnostics : nullptr;
    if (stats)
        stats->hasMergeDiagnostics = true;

    if (GeometryExperiments::kSkipStlMergeCoplanarFaces)
    {
        if (diagOut != nullptr)
            scene->CollectCoplanarMergeTopology(solid, diagOut);
        return;
    }

#if defined(CAD_CGAL_PLANAR_REMESH_EXPERIMENT_ENABLED)
    if (GeometryExperiments::kUseCgalRemeshPlanarPatchesForStl &&
        STLCgalPlanarExperiment::TryRemeshPlanarPatchesReplacingSolidFaces(scene, solid, diagOut))
        return;
#endif

    scene->MergeCoplanarFaces(solid, diagOut);
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

static bool ImportBinary(std::ifstream &file, Scene *scene, uint32_t triangleCount, STLImportStats *stats)
{
    using Clock = std::chrono::steady_clock;
    const Clock::time_point tStart = Clock::now();
    std::map<glm::dvec3, Point *, Vec3Compare> pointMap;
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
    }
    const double weldEps = StlWeldEpsilonFromDiagonal(glm::length(boundMax - boundMin));
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
            pts[v] = GetOrCreatePoint(scene, pointMap, pos, weldEps);
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
    }

    double parseMs = std::chrono::duration<double, std::milli>(Clock::now() - tStart).count();
    double mergeMs = 0.0;
    if (!faces.empty())
    {
        const Clock::time_point tMergeStart = Clock::now();
        Solid *solid = scene->CreateSolid(faces);
        MergeStlCoplanarMaybe(scene, solid, stats);
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
    return true;
}

static bool ImportASCII(std::ifstream &file, Scene *scene, STLImportStats *stats)
{
    using Clock = std::chrono::steady_clock;
    const Clock::time_point tStart = Clock::now();
    std::map<glm::dvec3, Point *, Vec3Compare> pointMap;
    std::vector<Face *> faces;
    const double weldEps = 1e-6; // ASCII path: no bbox pre-scan; conservative absolute snap

    std::string line;
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
    }

    double parseMs = std::chrono::duration<double, std::milli>(Clock::now() - tStart).count();
    double mergeMs = 0.0;
    if (!faces.empty())
    {
        const Clock::time_point tMergeStart = Clock::now();
        Solid *solid = scene->CreateSolid(faces);
        MergeStlCoplanarMaybe(scene, solid, stats);
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

bool STLImport::Import(const std::string &filePath, Scene *scene, STLImportStats *stats)
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open())
        return LOG_FALSE("Failed to open STL file: " + filePath);

    uint32_t triangleCount = 0;
    if (IsBinarySTL(file, triangleCount))
    {
        LOG_DESC("Importing binary STL: " + filePath)
        return ImportBinary(file, scene, triangleCount, stats);
    }

    file.clear();
    file.seekg(0);
    file.close();

    file.open(filePath);
    if (!file.is_open())
        return LOG_FALSE("Failed to reopen STL file as text: " + filePath);

    LOG_DESC("Importing ASCII STL: " + filePath)
    return ImportASCII(file, scene, stats);
}
