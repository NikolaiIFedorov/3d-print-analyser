#include "CalibDistanceType.hpp"

#include "scene.hpp"

#include <cmath>
#include <limits>

namespace CalibrateDistance
{

static double SceneMinZ(const Scene &scene)
{
    double zMin = std::numeric_limits<double>::max();

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
                zMin = std::min(zMin, std::min(p0.z, p1.z));
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

    return zMin;
}

void RebuildHoleEdgeSet(const Scene &scene, std::unordered_set<const Edge *> &out)
{
    out.clear();

    auto scanFace = [&](const Face *f)
    {
        if (f == nullptr || f->loops.size() < 2)
            return;
        for (size_t li = 1; li < f->loops.size(); ++li)
        {
            for (const auto &oe : f->loops[li])
            {
                if (oe.edge != nullptr)
                    out.insert(oe.edge);
            }
        }
    };

    for (const Solid &solid : scene.solids)
    {
        for (const Face *f : solid.faces)
            scanFace(f);
    }
    for (const Face &f : scene.faces)
        scanFace(&f);
}

bool FaceInFirstLayerSlab(const Face *face, const Scene *scene, double layerHeightMm)
{
    if (face == nullptr || scene == nullptr || layerHeightMm <= 0.0)
        return false;

    const double sceneMinZ = SceneMinZ(*scene);
    if (sceneMinZ == std::numeric_limits<double>::max())
        return false;

    const double topZ = sceneMinZ + layerHeightMm;
    constexpr double kEps = 1e-5;

    double fMinZ = std::numeric_limits<double>::max();
    double fMaxZ = std::numeric_limits<double>::lowest();
    for (const auto &loop : face->loops)
    {
        for (const auto &oe : loop)
        {
            const glm::dvec3 p0 = oe.GetStartPosition();
            const glm::dvec3 p1 = oe.GetEndPosition();
            fMinZ = std::min(fMinZ, std::min(p0.z, p1.z));
            fMaxZ = std::max(fMaxZ, std::max(p0.z, p1.z));
        }
    }

    return fMinZ >= sceneMinZ - kEps && fMaxZ <= topZ + kEps;
}

bool FaceQualifiesAsHole(const Face *face, const std::unordered_set<const Edge *> &holeEdges)
{
    if (face == nullptr)
        return false;

    if (face->loops.size() >= 3)
        return true;

    for (const auto &loop : face->loops)
    {
        for (const auto &oe : loop)
        {
            if (oe.edge != nullptr && holeEdges.count(oe.edge) != 0)
                return true;
        }
    }
    return false;
}

CalibWorkflow ClassifyFace(const Face *face, const Scene *scene, double layerHeightMm,
                           const std::unordered_set<const Edge *> &holeEdges)
{
    if (face == nullptr || scene == nullptr)
        return CalibWorkflow::None;

    if (FaceInFirstLayerSlab(face, scene, layerHeightMm))
        return CalibWorkflow::ElephantFoot;
    if (FaceQualifiesAsHole(face, holeEdges))
        return CalibWorkflow::Hole;
    return CalibWorkflow::Contour;
}

CalibWorkflow CombinePickedFaces(const Face *a, const Face *b, const Scene *scene, double layerHeightMm,
                                 const std::unordered_set<const Edge *> &holeEdges)
{
    if (a == nullptr && b == nullptr)
        return CalibWorkflow::None;
    if (b == nullptr)
        return ClassifyFace(a, scene, layerHeightMm, holeEdges);
    if (a == nullptr)
        return ClassifyFace(b, scene, layerHeightMm, holeEdges);

    const CalibWorkflow ca = ClassifyFace(a, scene, layerHeightMm, holeEdges);
    const CalibWorkflow cb = ClassifyFace(b, scene, layerHeightMm, holeEdges);

    if (ca == CalibWorkflow::ElephantFoot || cb == CalibWorkflow::ElephantFoot)
        return CalibWorkflow::ElephantFoot;
    if (ca == CalibWorkflow::Hole || cb == CalibWorkflow::Hole)
        return CalibWorkflow::Hole;
    if (ca == CalibWorkflow::Contour || cb == CalibWorkflow::Contour)
        return CalibWorkflow::Contour;
    return CalibWorkflow::None;
}

} // namespace CalibrateDistance
