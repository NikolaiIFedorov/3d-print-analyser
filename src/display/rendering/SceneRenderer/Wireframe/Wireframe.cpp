#include "Wireframe.hpp"
#include "rendering/SceneLighting.hpp"
#include "utils/utils.hpp"
#include <unordered_map>

namespace
{
glm::vec3 BrighterAdjacentFaceNormal(const Edge *edge, const glm::vec3 &lightDirUnit)
{
    if (edge == nullptr || edge->dependencies.empty())
        return glm::vec3(0.0f);

    const Face *bestFace = nullptr;
    float bestScore = -1.0f;
    for (Face *f : edge->dependencies)
    {
        if (f == nullptr)
            continue;
        glm::vec3 n = glm::vec3(f->GetSurface().GetNormal());
        const float len = glm::length(n);
        if (!(len > 1e-10f))
            continue;
        n /= len;
        const float score = glm::max(0.0f, glm::dot(n, lightDirUnit));
        if (score > bestScore)
        {
            bestScore = score;
            bestFace = f;
        }
    }
    if (bestFace == nullptr)
        return glm::vec3(0.0f);
    glm::vec3 n = glm::vec3(bestFace->GetSurface().GetNormal());
    const float len = glm::length(n);
    if (!(len > 1e-10f))
        return glm::vec3(0.0f);
    return n / len;
}
} // namespace

void Wireframe::Generate(Scene *scene, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices, const AnalysisResults *results) const
{
    for (const Solid &solid : scene->solids)
        GenerateSolid(&solid, vertices, indices, results);

    GenerateLoose(scene, vertices, indices);
}

void Wireframe::GenerateSolid(const Solid *solid, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices,
                              const AnalysisResults *results) const
{
    AddSolid(solid, vertices, indices, results);
}

void Wireframe::GenerateLoose(Scene *scene, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices) const
{
    for (const Face &face : scene->faces)
        AddFace(&face, vertices, indices, false);

    for (const Edge &edge : scene->edges)
    {
        if (edge.startPoint == nullptr || edge.endPoint == nullptr)
            continue;
        AddEdge(&edge, vertices, indices, false);
    }

    for (const Point &point : scene->points)
        AddPoint(&point, vertices, indices, false);
}

void Wireframe::AddPoint(const Point *point,
                         std::vector<Vertex> &vertices,
                         std::vector<uint32_t> &indices, bool isEdge) const
{
    // TODO: Add point
}

void Wireframe::AddEdge(const Edge *edge,
                        std::vector<Vertex> &vertices,
                        std::vector<uint32_t> &indices, bool isFace,
                        const glm::vec3 *colorOverride) const
{
    if (!edge->dependencies.empty() && !isFace)
        return;

    const glm::vec3 color = colorOverride ? *colorOverride : Color::GetEdge();
    const glm::vec3 litNormal =
        isFace ? BrighterAdjacentFaceNormal(edge, SceneLighting::DirectionalLightDirWorld()) : glm::vec3(0.0f);
    if (edge->curve == nullptr)
        AddLineEdge(edge, vertices, indices, color, litNormal);
    else
        AddCurvedEdge(edge, vertices, indices, color, litNormal);
}

void Wireframe::AddLineEdge(const Edge *edge,
                            std::vector<Vertex> &vertices,
                            std::vector<uint32_t> &indices,
                            const glm::vec3 &color,
                            const glm::vec3 &litNormal) const
{
    const Point *p0 = edge->startPoint;
    const Point *p1 = edge->endPoint;

    uint32_t baseIndex = vertices.size();

    Vertex v0, v1;
    v0.position = glm::vec3(p0->position);
    v0.color = color;
    v0.normal = litNormal;

    v1.position = glm::vec3(p1->position);
    v1.color = color;
    v1.normal = litNormal;

    vertices.push_back(v0);
    vertices.push_back(v1);

    indices.push_back(baseIndex);
    indices.push_back(baseIndex + 1);
}

void Wireframe::AddCurvedEdge(const Edge *edge,
                              std::vector<Vertex> &vertices,
                              std::vector<uint32_t> &indices,
                              const glm::vec3 &color,
                              const glm::vec3 &litNormal) const
{
    const Point *p0 = edge->startPoint;
    if (p0 == nullptr)
        return;

    const Point *p1 = edge->endPoint;
    if (p1 == nullptr)
        return;

    TessellateCurve(edge->curve, p0->position, p1->position, vertices, indices, color, litNormal);
}

void Wireframe::AddFace(const Face *face,
                        std::vector<Vertex> &vertices,
                        std::vector<uint32_t> &indices, bool isSolid) const
{
    if (face->dependency != nullptr && !isSolid)
        return;

    for (const auto &loop : face->loops)
    {
        for (const auto &orientedEdge : loop)
            AddEdge(orientedEdge.edge, vertices, indices, true);
    }
}

void Wireframe::AddSolid(const Solid *solid,
                         std::vector<Vertex> &vertices,
                         std::vector<uint32_t> &indices, const AnalysisResults *results) const
{
    if (results)
    {
        // Build a map from edge pointer to flaw color for O(1) lookup
        std::unordered_map<const Edge *, glm::vec3> edgeColors;
        auto it = results->edgeFlaws.find(solid);
        if (it != results->edgeFlaws.end())
        {
            for (const auto &ef : it->second)
                edgeColors[ef.edge] = glm::vec3(Color::GetEdge(ef.flaw));
        }

        for (auto face : solid->faces)
        {
            for (const auto &loop : face->loops)
            {
                for (const auto &orientedEdge : loop)
                {
                    auto colorIt = edgeColors.find(orientedEdge.edge);
                    const glm::vec3 *colorOverride = (colorIt != edgeColors.end()) ? &colorIt->second : nullptr;
                    AddEdge(orientedEdge.edge, vertices, indices, true, colorOverride);
                }
            }
        }
    }
    else
    {
        for (auto face : solid->faces)
            AddFace(face, vertices, indices, true);
    }
}

void Wireframe::TessellateCurve(const Curve *curve,
                                const glm::dvec3 &start,
                                const glm::dvec3 &end,
                                std::vector<Vertex> &vertices,
                                std::vector<uint32_t> &indices,
                                const glm::vec3 &color,
                                const glm::vec3 &litNormal) const
{
    int segments = 16; // TODO: Make adaptive segments

    for (int i = 0; i < segments; i += 1)
    {
        double t0 = (double)i / segments;
        double t1 = (double)(i + 1) / segments;

        glm::dvec3 p0 = curve->Evaluate(t0, start, end);
        glm::dvec3 p1 = curve->Evaluate(t1, start, end);

        uint32_t baseIndex = vertices.size();

        vertices.push_back({glm::vec3(p0), color, litNormal});
        vertices.push_back({glm::vec3(p1), color, litNormal});
        indices.push_back(baseIndex);

        indices.push_back(baseIndex + 1);
    }
}
