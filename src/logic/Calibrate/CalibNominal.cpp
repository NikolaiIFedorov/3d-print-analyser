#include "CalibNominal.hpp"

#include "CalibDistanceType.hpp"
#include "Geometry/Edge.hpp"
#include "Geometry/Face.hpp"
#include "Geometry/OrientedEdge.hpp"

#include <cmath>
#include <cstddef>

namespace CalibrateNominal
{

namespace
{

[[nodiscard]] glm::dvec3 EdgeChordMidpoint(const Edge *e)
{
    if (e == nullptr || e->startPoint == nullptr || e->endPoint == nullptr)
        return glm::dvec3(0.0);
    return 0.5 * (e->startPoint->position + e->endPoint->position);
}

[[nodiscard]] glm::dvec3 EdgeChordDirectionUnit(const Edge *e)
{
    if (e == nullptr || e->startPoint == nullptr || e->endPoint == nullptr)
        return glm::dvec3(0.0);
    glm::dvec3 d = e->endPoint->position - e->startPoint->position;
    const double len = glm::length(d);
    if (len < 1e-12)
        return glm::dvec3(0.0);
    return d / len;
}

} // namespace

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

bool EdgeBelongsToFace(const Edge *e, const Face *f)
{
    if (e == nullptr || f == nullptr)
        return false;
    for (const auto &loop : f->loops)
    {
        for (const auto &oe : loop)
        {
            if (oe.edge == e)
                return true;
        }
    }
    return false;
}

bool EdgesAreParallelForCalib(const Edge *a, const Edge *b)
{
    constexpr double kMinAbsDot = 0.985;
    const glm::dvec3 da = EdgeChordDirectionUnit(a);
    const glm::dvec3 db = EdgeChordDirectionUnit(b);
    if (glm::length(da) < 0.5 || glm::length(db) < 0.5)
        return false;
    return std::abs(glm::dot(da, db)) >= kMinAbsDot;
}

SpanPreview SpanPreviewBetweenParallelEdgesOnFace(const Face *face, const Edge *eA, const Edge *eB)
{
    SpanPreview out;
    if (face == nullptr || eA == nullptr || eB == nullptr || eA == eB)
        return out;
    if (!face->GetSurface().IsPlanar())
        return out;
    if (!EdgeBelongsToFace(eA, face) || !EdgeBelongsToFace(eB, face))
        return out;
    if (!EdgesAreParallelForCalib(eA, eB))
        return out;

    const glm::dvec3 u = EdgeChordDirectionUnit(eA);
    const glm::dvec3 pa = EdgeChordMidpoint(eA);
    const glm::dvec3 pb = EdgeChordMidpoint(eB);

    const glm::dvec3 c1 = pa + u * glm::dot(pb - pa, u);
    const glm::dvec3 c2 = pb;
    const double span = glm::length(c2 - c1);
    if (span < 1e-6)
        return out;

    out.nominalMm = static_cast<float>(span);
    out.valid = true;
    out.p0 = c1;
    out.p1 = c2;
    return out;
}

SpanResult SpanBetweenParallelEdgesOnFace(const Face *face, const Edge *eA, const Edge *eB)
{
    const SpanPreview p = SpanPreviewBetweenParallelEdgesOnFace(face, eA, eB);
    return {p.nominalMm, p.valid};
}

bool NominalSpanPerpendicularToBuild(const Face *a, const Face *b, const glm::dvec3 &buildDirWorld)
{
    const SpanPreview sp = SpanPreviewBetweenFaces(a, b);
    if (!sp.valid)
        return false;

    const glm::dvec3 seg = sp.p1 - sp.p0;
    const double len = glm::length(seg);
    if (len < 1e-6)
        return false;

    const glm::dvec3 dir = seg / len;
    glm::dvec3 bd = glm::normalize(buildDirWorld);
    if (!std::isfinite(bd.x) || glm::length(bd) < 1e-12)
        bd = glm::dvec3(0.0, 0.0, 1.0);

    return std::abs(glm::dot(dir, bd)) <= CalibrateDistance::kCalibSpanMaxAbsDotBuildAxis;
}

} // namespace CalibrateNominal
