#include "STLImport.hpp"
#include "scene/scene.hpp"
#include "utils/log.hpp"

#include <fstream>
#include <array>
#include <map>
#include <cstring>

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

static Point *GetOrCreatePoint(Scene *scene, std::map<glm::dvec3, Point *, Vec3Compare> &pointMap, const glm::dvec3 &pos)
{
    auto it = pointMap.find(pos);
    if (it != pointMap.end())
        return it->second;

    Point *p = scene->CreatePoint(pos);
    pointMap[pos] = p;
    return p;
}

static bool ImportBinary(std::ifstream &file, Scene *scene, uint32_t triangleCount)
{
    std::map<glm::dvec3, Point *, Vec3Compare> pointMap;
    std::vector<Face *> faces;
    faces.reserve(triangleCount);

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
            pts[v] = GetOrCreatePoint(scene, pointMap, pos);
        }

        if (pts[0] == pts[1] || pts[1] == pts[2] || pts[0] == pts[2])
            continue;

        Edge *e1 = scene->CreateEdge(pts[0], pts[1]);
        Edge *e2 = scene->CreateEdge(pts[1], pts[2]);
        Edge *e3 = scene->CreateEdge(pts[2], pts[0]);

        Face *f = scene->CreateFace({{e1, e2, e3}});
        faces.push_back(f);
    }

    if (!faces.empty())
    {
        Solid *solid = scene->CreateSolid(faces);
        scene->MergeCoplanarFaces(solid);
    }

    return true;
}

static bool ImportASCII(std::ifstream &file, Scene *scene)
{
    std::map<glm::dvec3, Point *, Vec3Compare> pointMap;
    std::vector<Face *> faces;

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

            pts[v] = GetOrCreatePoint(scene, pointMap, glm::dvec3(x, y, z));
        }

        if (pts[0] == pts[1] || pts[1] == pts[2] || pts[0] == pts[2])
            continue;

        Edge *e1 = scene->CreateEdge(pts[0], pts[1]);
        Edge *e2 = scene->CreateEdge(pts[1], pts[2]);
        Edge *e3 = scene->CreateEdge(pts[2], pts[0]);

        Face *f = scene->CreateFace({{e1, e2, e3}});
        faces.push_back(f);
    }

    if (!faces.empty())
    {
        Solid *solid = scene->CreateSolid(faces);
        scene->MergeCoplanarFaces(solid);
    }

    return true;
}

bool STLImport::Import(const std::string &filePath, Scene *scene)
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open())
        return LOG_FALSE("Failed to open STL file: " + filePath);

    uint32_t triangleCount = 0;
    if (IsBinarySTL(file, triangleCount))
    {
        LOG_DESC("Importing binary STL: " + filePath)
        return ImportBinary(file, scene, triangleCount);
    }

    file.clear();
    file.seekg(0);
    file.close();

    file.open(filePath);
    if (!file.is_open())
        return LOG_FALSE("Failed to reopen STL file as text: " + filePath);

    LOG_DESC("Importing ASCII STL: " + filePath)
    return ImportASCII(file, scene);
}
