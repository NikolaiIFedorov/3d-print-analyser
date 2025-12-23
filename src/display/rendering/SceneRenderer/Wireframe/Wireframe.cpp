#include "Wireframe.hpp"

void Wireframe::Generate(const Scene &scene, const RenderBuffer &buffer, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices, int viewport[4]) const
{
    for (uint32_t id : buffer.GetForms())
    {
        Type type = Id::GetType(id);
        switch (type)
        {
        case Type::POINT:
            AddPoint(scene, id, vertices, indices);
            break;

        case Type::EDGE:
            AddEdge(scene, id, vertices, indices);
            break;

        case Type::FACE:
            AddFace(scene, id, vertices, indices);
            break;

        case Type::SOLID:
            AddSolid(scene, id, vertices, indices);
            break;

        default:
            break;
        }
    }
}

void Wireframe::AddPoint(const Scene &scene, uint32_t id,
                         std::vector<Vertex> &vertices,
                         std::vector<uint32_t> &indices) const
{
    // TODO: Add point
}

void Wireframe::AddEdge(const Scene &scene, uint32_t id,
                        std::vector<Vertex> &vertices,
                        std::vector<uint32_t> &indices) const
{
    const Edge *edge = scene.GetEdge(id);
    if (edge == nullptr)
    {
        return;
    }

    if (edge->curveId == 0)
    {
        AddStraightEdge(scene, edge, vertices, indices);
    }
    else
    {
        AddCurvedEdge(scene, edge, vertices, indices);
    }
}

void Wireframe::AddStraightEdge(const Scene &scene, const Edge *edge,
                                std::vector<Vertex> &vertices,
                                std::vector<uint32_t> &indices) const
{
    if (edge == nullptr)
    {
        return;
    }

    const Point *p0 = scene.GetPoint(edge->startPointId);
    const Point *p1 = scene.GetPoint(edge->endPointId);

    if (p0 == nullptr || p1 == nullptr)
    {
        return;
    }

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

void Wireframe::AddCurvedEdge(const Scene &scene, const Edge *edge,
                              std::vector<Vertex> &vertices,
                              std::vector<uint32_t> &indices) const
{
    const Curve *curve = scene.GetCurve(edge->curveId);
    if (curve == nullptr)
        return;

    const Point *p0 = scene.GetPoint(edge->startPointId);
    const Point *p1 = scene.GetPoint(edge->endPointId);

    if (p0 == nullptr || p1 == nullptr)
    {
        return;
    }

    TessellateCurve(curve, p0->position, p1->position, vertices, indices);
}

void Wireframe::AddFace(const Scene &scene, uint32_t id,
                        std::vector<Vertex> &vertices,
                        std::vector<uint32_t> &indices) const
{
    const Face *face = scene.GetFace(id);
    for (auto hole : face->loops)
    {
        for (auto edgeId : hole)
        {
            AddEdge(scene, edgeId, vertices, indices);
        }
    }
}

void Wireframe::AddSolid(const Scene &scene, uint32_t id,
                         std::vector<Vertex> &vertices,
                         std::vector<uint32_t> &indices) const
{
    const Solid *solid = scene.GetSolid(id);
    for (auto faceId : solid->faceIds)
    {
        AddFace(scene, faceId, vertices, indices);
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
