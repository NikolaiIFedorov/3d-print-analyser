#pragma once

#include <glm/glm.hpp>
#include <unordered_set>

struct Edge;
class Face;
struct Point;
class Scene;

/// Shown in the Calibrate tool.
/// `Hole` = **layer hole** (opening that appears in a slice ⟂ the build direction), not every face with an inner loop.
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

/// Inner-loop edges of **cap** faces whose normals align with `buildDirWorld` (holes in a printed layer).
void RebuildHoleCalibTopology(const Scene &scene, const glm::dvec3 &buildDirWorld,
                              std::unordered_set<const Edge *> &holeEdgesOut,
                              std::unordered_set<const Point *> &holeRingPointsOut);

/// Face lies within the first slice of thickness `layerHeightMm` along `buildDirWorld` from the scene extent.
bool FaceInFirstLayerSlab(const Face *face, const Scene *scene, double layerHeightMm,
                          const glm::dvec3 &buildDirWorld);

/// Layer hole: annular cap (≥2 loops, cap ∥ build dir) or face touching layer-hole inner-loop ring / edges.
bool FaceQualifiesAsHole(const Face *face, const glm::dvec3 &buildDirWorld,
                         const std::unordered_set<const Edge *> &holeEdges,
                         const std::unordered_set<const Point *> &holeRingPoints);

CalibWorkflow ClassifyFace(const Face *face, const Scene *scene, double layerHeightMm,
                           const glm::dvec3 &buildDirWorld,
                           const std::unordered_set<const Edge *> &holeEdges,
                           const std::unordered_set<const Point *> &holeRingPoints);

/// True unless the pair mixes contour vs layer-hole workflow.
inline bool CalibSecondPickWorkflowsCompatible(CalibWorkflow a, CalibWorkflow b)
{
    return !((a == CalibWorkflow::Contour && b == CalibWorkflow::Hole) ||
             (a == CalibWorkflow::Hole && b == CalibWorkflow::Contour));
}

CalibWorkflow CombinePickedFaces(const Face *a, const Face *b, const Scene *scene, double layerHeightMm,
                                   const glm::dvec3 &buildDirWorld,
                                   const std::unordered_set<const Edge *> &holeEdges,
                                   const std::unordered_set<const Point *> &holeRingPoints);

} // namespace CalibrateDistance
