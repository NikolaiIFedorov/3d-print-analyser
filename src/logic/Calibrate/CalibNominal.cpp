#include "CalibNominal.hpp"

#include "Geometry/Face.hpp"
#include "Geometry/OrientedEdge.hpp"

#include <cmath>
#include <cstddef>

namespace CalibrateNominal
{

bool NormalsAlignedForCalibPick(const Face *a, const Face *b)
{
    if (a == nullptr || b == nullptr || a == b)
        return false;

    const glm::dvec3 na = glm::normalize(a->GetSurface().GetNormal());
    const glm::dvec3 nb = glm::normalize(b->GetSurface().GetNormal());
    if (!std::isfinite(na.x) || !std::isfinite(nb.x))
        return false;

    return std::abs(glm::dot(na, nb)) >= kFaceNormalParallelAlignThreshold;
}

glm::dvec3 FaceCentroidWorld(const Face *f)
{
    if (f == nullptr)
        return glm::dvec3(0.0);
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

SpanPreview SpanPreviewBetweenFaces(const Face *a, const Face *b)
{
    SpanPreview out;
    if (a == nullptr || b == nullptr || a == b)
        return out;

    const glm::dvec3 na = glm::normalize(a->GetSurface().GetNormal());
    const glm::dvec3 nb = glm::normalize(b->GetSurface().GetNormal());
    if (!std::isfinite(na.x) || !std::isfinite(nb.x))
        return out;

    const glm::dvec3 ca = FaceCentroidWorld(a);
    const glm::dvec3 cb = FaceCentroidWorld(b);
    const glm::dvec3 delta = cb - ca;

    const double align = std::abs(glm::dot(na, nb));

    double spanMm = 0.0;
    if (align >= kFaceNormalParallelAlignThreshold)
    {
        const double signedAlong = glm::dot(delta, na);
        spanMm = std::abs(signedAlong);
        out.p0 = ca;
        out.p1 = ca + signedAlong * na;
    }
    else
    {
        spanMm = glm::length(delta);
        out.p0 = ca;
        out.p1 = cb;
    }

    if (spanMm < 1e-6)
        return out;

    out.nominalMm = static_cast<float>(spanMm);
    out.valid = true;
    return out;
}

SpanResult SpanBetweenFaces(const Face *a, const Face *b)
{
    const SpanPreview p = SpanPreviewBetweenFaces(a, b);
    return {p.nominalMm, p.valid};
}

} // namespace CalibrateNominal
