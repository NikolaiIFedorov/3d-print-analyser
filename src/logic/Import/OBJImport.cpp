#include "OBJImport.hpp"
#include "scene/scene.hpp"
#include "utils/log.hpp"

#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cstdint>

static int ParseVertexIndex(const std::string &token, int vertexCount)
{
    std::istringstream ss(token);
    int idx;
    ss >> idx;

    if (idx < 0)
        idx = vertexCount + idx + 1;

    return idx - 1; // convert to 0-based
}

bool OBJImport::Import(const std::string &filePath, Scene *scene, const ImportProgressCallback *progress)
{
    ReportImportProgress(progress, "Opening OBJ file...", 0.0f);
    std::ifstream file(filePath);
    if (!file.is_open())
        return LOG_FALSE("Failed to open OBJ file: " + filePath);

    LOG_DESC("Importing OBJ: " + filePath)

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    file.seekg(0);

    std::vector<Point *> points;
    std::vector<Face *> faces;

    std::string line;
    std::uint64_t lineIndex = 0;
    while (std::getline(file, line))
    {
        if (progress != nullptr && *progress && fileSize > 0 && (lineIndex++ % 512) == 0)
        {
            const std::streampos pos = file.tellg();
            if (pos != std::streampos(-1))
            {
                const std::streamoff posOffset = static_cast<std::streamoff>(pos);
                ReportImportProgress(
                    progress,
                    "Reading OBJ geometry...",
                    MapImportProgress(
                        std::clamp(static_cast<float>(static_cast<double>(posOffset) / static_cast<double>(fileSize)), 0.0f, 1.0f),
                        0.05f,
                        0.85f));
            }
        }

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

    ReportImportProgress(progress, "Reading OBJ geometry...", 0.85f);
    if (!faces.empty())
    {
        ReportImportProgress(progress, "Creating OBJ solid...", 0.85f);
        scene->CreateSolid(faces);
    }

    return true;
}
