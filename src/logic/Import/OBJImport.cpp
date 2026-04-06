#include "OBJImport.hpp"
#include "scene/scene.hpp"
#include "utils/log.hpp"

#include <fstream>
#include <sstream>
#include <vector>

static int ParseVertexIndex(const std::string &token, int vertexCount)
{
    std::istringstream ss(token);
    int idx;
    ss >> idx;

    if (idx < 0)
        idx = vertexCount + idx + 1;

    return idx - 1; // convert to 0-based
}

bool OBJImport::Import(const std::string &filePath, Scene *scene)
{
    std::ifstream file(filePath);
    if (!file.is_open())
        return LOG_FALSE("Failed to open OBJ file: " + filePath);

    LOG_DESC("Importing OBJ: " + filePath)

    std::vector<Point *> points;
    std::vector<Face *> faces;

    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
            continue;

        std::istringstream ss(line);
        std::string prefix;
        ss >> prefix;

        if (prefix == "v")
        {
            double x, y, z;
            if (!(ss >> x >> y >> z))
                continue;

            points.push_back(scene->CreatePoint(glm::dvec3(x, y, z)));
        }
        else if (prefix == "f")
        {
            std::vector<int> vertexIndices;
            std::string token;
            while (ss >> token)
            {
                // Extract vertex index from formats: v, v/vt, v/vt/vn, v//vn
                std::string vertPart = token.substr(0, token.find('/'));
                int idx = ParseVertexIndex(vertPart, static_cast<int>(points.size()));

                if (idx < 0 || idx >= static_cast<int>(points.size()))
                {
                    LOG_WARN("OBJ vertex index out of range: " + token)
                    vertexIndices.clear();
                    break;
                }
                vertexIndices.push_back(idx);
            }

            if (vertexIndices.size() < 3)
                continue;

            // Fan triangulation for n-gon faces
            Point *p0 = points[vertexIndices[0]];
            for (size_t i = 1; i + 1 < vertexIndices.size(); ++i)
            {
                Point *p1 = points[vertexIndices[i]];
                Point *p2 = points[vertexIndices[i + 1]];

                if (p0 == p1 || p1 == p2 || p0 == p2)
                    continue;

                Edge *e1 = scene->CreateEdge(p0, p1);
                Edge *e2 = scene->CreateEdge(p1, p2);
                Edge *e3 = scene->CreateEdge(p2, p0);

                faces.push_back(scene->CreateFace({{e1, e2, e3}}));
            }
        }
    }

    if (!faces.empty())
        scene->CreateSolid(faces);

    return true;
}
