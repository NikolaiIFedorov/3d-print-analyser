#include "Analysis.hpp"

Flaw Analysis::GetFlaw(uint32_t id, const Scene &scene)
{
    Flaw flaw = Flaw::OVERHANG;
    const Face *face = scene.GetFace(id);
    if (face == nullptr)
    {
        LOG_VOID("Face not found: " + Log::NumToStr(id));
        return Flaw::LOGIC_ERROR;
    }

    glm::dvec3 normal = CalculateFaceNormal(face, scene);
    float angle = CalculateOverhangAngle(normal);

    if (angle > criticalAngle)
    {
        return Flaw::OVERHANG;
    }

    return Flaw::NONE;
}

glm::dvec3 Analysis::CalculateFaceNormal(const Face *face, const Scene &scene)
{
    return face->GetPlanar().normal;
}

float Analysis::CalculateOverhangAngle(const glm::dvec3 &normal)
{
    glm::dvec3 buildDirection = {0.0, 0.0, 1.0};

    double dotProduct = glm::dot(normal, buildDirection);
    dotProduct = glm::clamp(dotProduct, -1.0, 1.0);

    double angle = glm::degrees(std::acos(dotProduct));

    return static_cast<float>(angle);
}

std::optional<glm::dvec2> Analysis::IntersectEdgeWithPlane(const Edge *edge,
                                                           const Scene &scene,
                                                           double z)
{
    if (edge == nullptr)
        return std::nullopt;

    const Point *p0 = scene.GetPoint(edge->startPointId);
    const Point *p1 = scene.GetPoint(edge->endPointId);

    if (p0 == nullptr || p1 == nullptr)
        return std::nullopt;

    glm::dvec3 start = p0->position;
    glm::dvec3 end = p1->position;

    double z0 = start.z;
    double z1 = end.z;

    // Edge doesn't cross this plane
    if ((z0 < z && z1 < z) || (z0 > z && z1 > z))
    {
        return std::nullopt;
    }

    // Edge is exactly on the plane (degenerate case)
    if (std::abs(z0 - z1) < 1e-9)
    {
        return std::nullopt;
    }

    double t = (z - z0) / (z1 - z0);

    glm::dvec3 intersection = start + t * (end - start);

    return glm::dvec2(intersection.x, intersection.y);
}

std::vector<glm::dvec2> Analysis::SliceFaceAtZ(const Face *face,
                                               const Scene &scene,
                                               double z)
{
    if (face == nullptr)
        return {};

    std::vector<glm::dvec2> intersectionPoints;

    const auto &outerLoop = face->GetOuterLoop(); // TODO: Add support for holes

    for (uint32_t edgeId : outerLoop)
    {
        const Edge *edge = scene.GetEdge(edgeId);
        auto intersection = IntersectEdgeWithPlane(edge, scene, z);

        if (intersection.has_value())
        {
            intersectionPoints.push_back(intersection.value());
        }
    }

    return intersectionPoints;
}