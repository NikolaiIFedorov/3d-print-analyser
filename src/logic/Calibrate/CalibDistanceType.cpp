#include "CalibDistanceType.hpp"

#include "scene.hpp"

#include <cmath>
#include <limits>

namespace CalibrateDistance
{
namespace
{

constexpr double kCapNormalAlignMinAbsDot = 0.985; // ~10° — cap face ∥ build direction

[[nodiscard]] glm::dvec3 NormalizeBuildDir(const glm::dvec3 &buildDirWorld)
{
    glm::dvec3 d = glm::normalize(buildDirWorld);
    if (!std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.z) || glm::length(d) < 1e-9)
        return glm::dvec3(0.0, 0.0, 1.0);
    return d;
}

[[nodiscard]] bool FaceCapParallelBuildDir(const Face *face, const glm::dvec3 &dirUnit)
{
    if (face == nullptr || !face->GetSurface().IsPlanar())
        return false;

    glm::dvec3 n = glm::normalize(face->GetSurface().GetNormal());
    if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z) || glm::length(n) < 1e-12)
        return false;
    return std::abs(glm::dot(n, dirUnit)) >= kCapNormalAlignMinAbsDot;
}

[[nodiscard]] double SceneMinAlongBuildDir(const Scene &scene, const glm::dvec3 &dirUnit)
{
    double tMin = std::numeric_limits<double>::max();

    auto considerFace = [&](const Face *f)
    {
        if (f == nullptr)
            return;
        for (const auto &loop : f->loops)
        {
            for (const auto &oe : loop)
            {
                const glm::dvec3 p0 = oe.GetStartPosition();
                const glm::dvec3 p1 = oe.GetEndPosition();
                tMin = std::min(tMin, std::min(glm::dot(p0, dirUnit), glm::dot(p1, dirUnit)));
            }
        }
    };

    for (const Solid &solid : scene.solids)
    {
        for (const Face *f : solid.faces)
            considerFace(f);
    }
    for (const Face &f : scene.faces)
        considerFace(&f);

    return tMin;
}

} // namespace

bool FaceInFirstLayerSlab(const Face *face, const Scene *scene, double layerHeightMm,
                          const glm::dvec3 &buildDirWorld)
{
    if (face == nullptr || scene == nullptr || layerHeightMm <= 0.0)
        return false;

    const glm::dvec3 d = NormalizeBuildDir(buildDirWorld);
    const double sceneMin = SceneMinAlongBuildDir(*scene, d);
    if (sceneMin == std::numeric_limits<double>::max())
        return false;

    const double top = sceneMin + layerHeightMm;
    constexpr double kEps = 1e-5;

    double fMin = std::numeric_limits<double>::max();
    double fMax = std::numeric_limits<double>::lowest();
    for (const auto &loop : face->loops)
    {
        for (const auto &oe : loop)
        {
            const glm::dvec3 p0 = oe.GetStartPosition();
            const glm::dvec3 p1 = oe.GetEndPosition();
            const double t0 = glm::dot(p0, d);
            const double t1 = glm::dot(p1, d);
            fMin = std::min(fMin, std::min(t0, t1));
            fMax = std::max(fMax, std::max(t0, t1));
        }
    }

    return fMin >= sceneMin - kEps && fMax <= top + kEps;
}

bool FaceQualifiesAsHole(const Face *face, const glm::dvec3 &buildDirWorld)
{
    if (face == nullptr || face->loops.size() < 2)
        return false;
    return FaceCapParallelBuildDir(face, NormalizeBuildDir(buildDirWorld));
}

CalibWorkflow ClassifyFace(const Face *face, const Scene *scene, double layerHeightMm,
                           const glm::dvec3 &buildDirWorld)
{
    if (face == nullptr || scene == nullptr)
        return CalibWorkflow::None;

    if (FaceInFirstLayerSlab(face, scene, layerHeightMm, buildDirWorld))
        return CalibWorkflow::ElephantFoot;
    if (FaceQualifiesAsHole(face, buildDirWorld))
        return CalibWorkflow::Hole;
    return CalibWorkflow::Contour;
}

CalibWorkflow CombinePickedFaces(const Face *a, const Face *b, const Scene *scene, double layerHeightMm,
                                 const glm::dvec3 &buildDirWorld)
{
    if (a == nullptr && b == nullptr)
        return CalibWorkflow::None;
    if (b == nullptr)
        return ClassifyFace(a, scene, layerHeightMm, buildDirWorld);
    if (a == nullptr)
        return ClassifyFace(b, scene, layerHeightMm, buildDirWorld);

    const CalibWorkflow ca = ClassifyFace(a, scene, layerHeightMm, buildDirWorld);
    const CalibWorkflow cb = ClassifyFace(b, scene, layerHeightMm, buildDirWorld);

    if (!CalibSecondPickWorkflowsCompatible(ca, cb))
        return CalibWorkflow::None;

    if (ca == CalibWorkflow::ElephantFoot || cb == CalibWorkflow::ElephantFoot)
        return CalibWorkflow::ElephantFoot;
    if (ca == CalibWorkflow::Hole || cb == CalibWorkflow::Hole)
        return CalibWorkflow::Hole;
    if (ca == CalibWorkflow::Contour || cb == CalibWorkflow::Contour)
        return CalibWorkflow::Contour;
    return CalibWorkflow::None;
}

} // namespace CalibrateDistance
