#include "CalibNominal.hpp"

#include "Geometry/Face.hpp"
#include "Geometry/OrientedEdge.hpp"

#include <cmath>
#include <cstddef>

namespace CalibrateNominal
{

static glm::dvec3 FaceCentroid(const Face *f)
{
    glm::dvec3 sum(0.0);
    size_t count = 0;
    for (const auto &loop : f->loops)
    {
        for (const auto &oe : loop)
        {
            sum += oe.GetStartPosition();
            ++count;
        }
    }
    if (count == 0)
        return glm::dvec3(0.0);
    return sum / static_cast<double>(count);
}

SpanResult SpanBetweenFaces(const Face *a, const Face *b)
{
    SpanResult out;
    if (a == nullptr || b == nullptr || a == b)
        return out;

    const glm::dvec3 na = glm::normalize(a->GetSurface().GetNormal());
    const glm::dvec3 nb = glm::normalize(b->GetSurface().GetNormal());
    if (!std::isfinite(na.x) || !std::isfinite(nb.x))
        return out;

    const glm::dvec3 ca = FaceCentroid(a);
    const glm::dvec3 cb = FaceCentroid(b);
    const glm::dvec3 delta = cb - ca;

    const double align = std::abs(glm::dot(na, nb));
    constexpr double kParallel = 0.75;

    double spanMm = 0.0;
    if (align >= kParallel)
        spanMm = std::abs(glm::dot(delta, na));
    else
        spanMm = glm::length(delta);

    if (spanMm < 1e-6)
        return out;

    out.nominalMm = static_cast<float>(spanMm);
    out.valid = true;
    return out;
}

} // namespace CalibrateNominal
