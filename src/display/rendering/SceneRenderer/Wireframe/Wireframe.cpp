#include "Wireframe.hpp"
#include "utils/utils.hpp"
#include <unordered_set>

void Wireframe::Generate(Scene *scene, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices, const AnalysisResults *results) const
{
    for (const Solid &solid : scene->solids)
        AddSolid(&solid, vertices, indices, results);

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
                        std::vector<uint32_t> &indices, bool isFace) const
{
    if (!edge->dependencies.empty() && !isFace)
        return;

    if (edge->curve == nullptr)
        AddLineEdge(edge, vertices, indices);
    else
        AddCurvedEdge(edge, vertices, indices);
}

void Wireframe::AddLineEdge(const Edge *edge,
                            std::vector<Vertex> &vertices,
                            std::vector<uint32_t> &indices) const
{
    const Point *p0 = edge->startPoint;
    const Point *p1 = edge->endPoint;

    uint32_t baseIndex = vertices.size();

    Vertex v0, v1;
    v0.position = glm::vec3(p0->position);
    v0.color = Color::GetEdge();

    v1.position = glm::vec3(p1->position);
    v1.color = Color::GetEdge();

    vertices.push_back(v0);
    vertices.push_back(v1);

    indices.push_back(baseIndex);
    indices.push_back(baseIndex + 1);
}

void Wireframe::AddCurvedEdge(const Edge *edge,
                              std::vector<Vertex> &vertices,
                              std::vector<uint32_t> &indices) const
{
    const Point *p0 = edge->startPoint;
    if (p0 == nullptr)
        return;

    const Point *p1 = edge->endPoint;
    if (p1 == nullptr)
        return;

    TessellateCurve(edge->curve, p0->position, p1->position, vertices, indices);
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
        std::unordered_set<const Edge *> flawedEdges;
        auto it = results->edgeFlaws.find(solid);
        if (it != results->edgeFlaws.end())
        {
            for (const auto &ef : it->second)
                flawedEdges.insert(ef.edge);
        }

        for (auto face : solid->faces)
        {
            for (const auto &loop : face->loops)
            {
                for (const auto &orientedEdge : loop)
                {
                    if (flawedEdges.count(orientedEdge.edge))
                        AddEdge(orientedEdge.edge, vertices, indices, true);
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
                                std::vector<uint32_t> &indices) const
{
    int segments = 16; // TODO: Make adaptive segments

    for (int i = 0; i < segments; i += 1)
    {
        double t0 = (double)i / segments;
        double t1 = (double)(i + 1) / segments;

        glm::dvec3 p0 = curve->Evaluate(t0, start, end);
        glm::dvec3 p1 = curve->Evaluate(t1, start, end);

        uint32_t baseIndex = vertices.size();

        vertices.push_back({glm::vec3(p0), Color::GetEdge()});
        vertices.push_back({glm::vec3(p1), Color::GetEdge()});
        indices.push_back(baseIndex);

        indices.push_back(baseIndex + 1);
    }
}
