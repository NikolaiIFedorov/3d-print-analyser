#include "Wireframe.hpp"

void Wireframe::Generate(const RenderBuffer &buffer, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices) const
{
    for (FormPtr form : buffer.GetForms())
    {
        switch (form.index())
        {
        case 0:
        {
            const Point *point = std::get<Point *>(form);
            AddPoint(point, vertices, indices);
            break;
        }
        case 1:
        {
            const Edge *edge = std::get<Edge *>(form);
            AddEdge(edge, vertices, indices);
            break;
        }
        case 3:
        {
            const Face *face = std::get<Face *>(form);
            AddFace(face, vertices, indices);
            break;
        }

        case 4:
        {
            const Solid *solid = std::get<Solid *>(form);
            AddSolid(solid, vertices, indices);
            break;
        }
        default:
            break;
        }
    }
}

void Wireframe::AddPoint(const Point *point,
                         std::vector<Vertex> &vertices,
                         std::vector<uint32_t> &indices) const
{
    // TODO: Add point
}

void Wireframe::AddEdge(const Edge *edge,
                        std::vector<Vertex> &vertices,
                        std::vector<uint32_t> &indices) const
{

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

void Wireframe::AddLayers(const std::vector<Layer> &layers,
                          std::vector<Vertex> &vertices,
                          std::vector<uint32_t> &indices) const
{
    for (const auto &layer : layers)
        AddLayer(layer, vertices, indices);
}

void Wireframe::AddLayer(const Layer &layer,
                         std::vector<Vertex> &vertices,
                         std::vector<uint32_t> &indices) const
{
    const auto &segments = layer.segments;
    for (const auto &segment : segments)
    {
        const glm::dvec3 *p0 = &segment.a;
        const glm::dvec3 *p1 = &segment.b;

        uint32_t baseIndex = vertices.size();

        Vertex v0, v1;
        v0.position = glm::vec3(*p0);
        v0.color = Color::GetEdge(layer.flaw);

        v1.position = glm::vec3(*p1);
        v1.color = Color::GetEdge(layer.flaw);

        vertices.push_back(v0);
        vertices.push_back(v1);

        indices.push_back(baseIndex);
        indices.push_back(baseIndex + 1);
    }
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
                        std::vector<uint32_t> &indices) const
{
    for (const auto &loop : face->loops)
    {
        for (const auto &orientedEdge : loop)
            AddEdge(orientedEdge.edge, vertices, indices);
    }
}

void Wireframe::AddSolid(const Solid *solid,
                         std::vector<Vertex> &vertices,
                         std::vector<uint32_t> &indices) const
{
    for (auto face : solid->faces)
        AddFace(face, vertices, indices);

    AddLayers(Analysis::Instance().FlawSolid(solid), vertices, indices);
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
