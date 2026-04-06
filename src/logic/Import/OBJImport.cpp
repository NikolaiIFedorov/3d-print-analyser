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

            // Create edge loop around the full polygon
            std::vector<Edge *> edgeLoop;
            bool valid = true;
            for (size_t i = 0; i < vertexIndices.size(); ++i)
            {
                Point *p0 = points[vertexIndices[i]];
                Point *p1 = points[vertexIndices[(i + 1) % vertexIndices.size()]];

                if (p0 == p1)
                {
                    valid = false;
                    break;
                }

                edgeLoop.push_back(scene->CreateEdge(p0, p1));
            }

            if (valid && edgeLoop.size() >= 3)
                faces.push_back(scene->CreateFace({edgeLoop}));
        }
    }

    if (!faces.empty())
        scene->CreateSolid(faces);

    return true;
}
