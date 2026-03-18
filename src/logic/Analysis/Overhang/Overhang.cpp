#include "Overhang.hpp"
#include "utils/log.hpp"

std::optional<Flaw> Overhang::Analyze(const Face *face) const
{
    const double OVERHANG_ANGLE_DEGREES = 45.0;
    const double cos_threshold = std::cos(glm::radians(OVERHANG_ANGLE_DEGREES));
    const glm::dvec3 build_direction(0.0, 0.0, 1.0);

    if (!face->GetSurface().IsPlanar())
        return std::nullopt;

    glm::dvec3 normal = glm::normalize(face->GetSurface().GetNormal());

    double dot_product = glm::dot(normal, build_direction);

    if (dot_product > cos_threshold)
        return std::nullopt;

    return Flaw::OVERHANG;
}