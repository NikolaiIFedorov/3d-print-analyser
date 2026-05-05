#pragma once

#include <glm/glm.hpp>
#include <unordered_set>

struct Edge;
class Face;
class Scene;

/// Shown in the Calibrate tool.
/// `Hole` = **layer hole**: annular cap (∥ build, ≥2 loops) or sidewall sharing a **layer-hole inner loop
/// edge** with a wall not parallel to build — not vertex-only adjacency.
enum class CalibWorkflow
{
    None,
    Contour,
    Hole,
    ElephantFoot
};

namespace CalibrateDistance
{

/// World unit vector along which layers stack (layer planes ⟂ this). +Z until a user setting exists.
inline glm::dvec3 DefaultCalibrateBuildDirection()
{
    return glm::dvec3(0.0, 0.0, 1.0);
}

/// Inner-loop edges of planar faces with ≥2 boundary loops (openings); framing face may be tilted vs build.
/// Annular cap vs sidewall is decided in `FaceQualifiesAsHole`.
void RebuildHoleCalibTopology(const Scene &scene, const glm::dvec3 &buildDirWorld,
                              std::unordered_set<const Edge *> &holeInnerEdgesOut);

/// Face lies within the first slice of thickness `layerHeightMm` along `buildDirWorld` from the scene extent.
bool FaceInFirstLayerSlab(const Face *face, const Scene *scene, double layerHeightMm,
                          const glm::dvec3 &buildDirWorld);

bool FaceQualifiesAsHole(const Face *face, const glm::dvec3 &buildDirWorld,
                         const std::unordered_set<const Edge *> &layerHoleInnerEdges);

CalibWorkflow ClassifyFace(const Face *face, const Scene *scene, double layerHeightMm,
                           const glm::dvec3 &buildDirWorld,
                           const std::unordered_set<const Edge *> &layerHoleInnerEdges);

/// True unless the pair mixes contour vs layer-hole workflow.
inline bool CalibSecondPickWorkflowsCompatible(CalibWorkflow a, CalibWorkflow b)
{
    return !((a == CalibWorkflow::Contour && b == CalibWorkflow::Hole) ||
             (a == CalibWorkflow::Hole && b == CalibWorkflow::Contour));
}

CalibWorkflow CombinePickedFaces(const Face *a, const Face *b, const Scene *scene, double layerHeightMm,
                                 const glm::dvec3 &buildDirWorld,
                                 const std::unordered_set<const Edge *> &layerHoleInnerEdges);

} // namespace CalibrateDistance
