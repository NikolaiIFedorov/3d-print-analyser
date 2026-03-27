#include "patch.hpp"
#include "utils/log.hpp"

static void CreatePlaneCoordinateSystem(const glm::dvec3 &normal,
                                        glm::dvec3 &uAxis,
                                        glm::dvec3 &vAxis)
{
    glm::dvec3 arbitrary;
    if (std::abs(normal.x) < 0.9)
    {
        arbitrary = glm::dvec3(1, 0, 0);
    }
    else
    {
        arbitrary = glm::dvec3(0, 1, 0);
    }

    uAxis = glm::normalize(glm::cross(normal, arbitrary));
    vAxis = glm::normalize(glm::cross(normal, uAxis));
}

static glm::dvec2 ProjectPointToPlane(const glm::dvec3 &point3D,
                                      const glm::dvec3 &origin,
                                      const glm::dvec3 &uAxis,
                                      const glm::dvec3 &vAxis)
{
    glm::dvec3 relativePos = point3D - origin;
    return glm::dvec2(
        glm::dot(relativePos, uAxis),
        glm::dot(relativePos, vAxis));
}

void Patch::Generate(Scene *scene, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices, int viewport[4]) const
{
    for (const Solid &solid : scene->solids)
        AddSolid(&solid, vertices, indices);

    for (const Face &face : scene->faces)
        AddFace(&face, vertices, indices, false);
}

void Patch::AddFace(const Face *face,
                    std::vector<Vertex> &vertices,
                    std::vector<uint32_t> &indices, bool isSolid) const
{
    if (face->dependency != nullptr && !isSolid)
        return;

    glm::dvec3 faceNormal = face->GetSurface().GetNormal();

    glm::dvec3 uAxis, vAxis;
    CreatePlaneCoordinateSystem(faceNormal, uAxis, vAxis);

    using Point2D = std::array<double, 2>;
    std::vector<std::vector<Point2D>> polygon;
    std::vector<std::vector<glm::dvec3>> allLoopPositions;

    glm::dvec3 projectionOrigin(0, 0, 0);
    bool originSet = false;

    for (const auto &edgeLoop : face->loops)
    {
        std::vector<glm::dvec3> loopPositions;

        for (const auto &orientedEdge : edgeLoop)
        {
            // Edges are already oriented correctly by Face constructor
            const Point *p0 = orientedEdge.GetStart();
            const Point *p1 = orientedEdge.GetEnd();

            // Set projection origin from first point
            if (!originSet)
            {
                projectionOrigin = p0->position;
                originSet = true;
            }

            if (orientedEdge.edge->curve == nullptr)
            {
                loopPositions.push_back(p0->position);
            }
            else
            {
                const Curve *curve = orientedEdge.edge->curve;

                std::vector<glm::dvec3> tessellatedPoints = TessellateCurveToPoints(
                    curve,
                    orientedEdge.GetStartPosition(),
                    orientedEdge.GetEndPosition(),
                    16);

                for (size_t j = 0; j < tessellatedPoints.size() - 1; j++)
                {
                    loopPositions.push_back(tessellatedPoints[j]);
                }
            }
        }

        allLoopPositions.push_back(loopPositions);

        std::vector<Point2D> loop2D;
        for (const glm::dvec3 &pos : loopPositions)
        {
            glm::dvec2 pos2D = ProjectPointToPlane(pos, projectionOrigin, uAxis, vAxis);
            loop2D.push_back({pos2D.x, pos2D.y});
        }
        polygon.push_back(loop2D);
    }

    if (polygon.empty())
        return;

    std::vector<uint32_t> triangleIndices = mapbox::earcut<uint32_t>(polygon);

    if (triangleIndices.empty())
    {
        LOG_WARN("Earcut produced no indices for face");
        return;
    }

    uint32_t baseVertexIndex = vertices.size();

    for (const auto &loopPositions : allLoopPositions)
    {
        for (const glm::dvec3 &pos : loopPositions)
        {
            Vertex v;
            v.position = glm::vec3(pos);
            v.color = Color::GetFace(face);

            vertices.push_back(v);
        }
    }

    for (uint32_t idx : triangleIndices)
    {
        indices.push_back(baseVertexIndex + idx);
    }
}

void Patch::AddSolid(const Solid *solid,
                     std::vector<Vertex> &vertices,
                     std::vector<uint32_t> &indices) const
{
    for (const Face *face : solid->faces)
    {
        AddFace(face, vertices, indices, true);
    }
}

std::vector<glm::dvec3> Patch::TessellateCurveToPoints(
    const Curve *curve,
    const glm::dvec3 &start,
    const glm::dvec3 &end,
    int segments) const
{
    std::vector<glm::dvec3> points;
    if (curve == nullptr)
    {
        points.push_back(start);
        points.push_back(end);
        return points;
    }

    for (int i = 0; i <= segments + 1; i++)
    {
        double t = (double)i / segments;
        glm::dvec3 p = curve->Evaluate(t, start, end);
        points.push_back(p);
    }
    return points;
}