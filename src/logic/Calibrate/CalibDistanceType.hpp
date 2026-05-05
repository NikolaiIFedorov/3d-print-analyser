#pragma once

#include <glm/glm.hpp>
#include <unordered_set>

struct Edge;
class Face;
class Scene;

/// Shown in the Calibrate tool.
/// `Hole` = stack-parallel annulus (≥2 loops on cap ∥ build) **or** ⊥ sidewall with ≥2 edges each witnessed
/// on an inner loop of such a cap (opening parallel slice planes — tunnel ∥ build). Contour/Hole picks gated ⊥ build in Display (elephant’s foot excepted).
/// `ElephantFoot` = two parallel **edges** on the same first-layer build-parallel cap (see Display picks).
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

/// Inner-loop edges of planar caps ∥ `buildDirWorld` (layer-plane openings). Slanted pockets excluded.
/// Annular cap vs sidewall is decided in `FaceQualifiesAsHole`.
void RebuildHoleCalibTopology(const Scene &scene, const glm::dvec3 &buildDirWorld,
                              std::unordered_set<const Edge *> &holeInnerEdgesOut);

/// Face lies within the first slice of thickness `layerHeightMm` along `buildDirWorld` from the scene extent.
bool FaceInFirstLayerSlab(const Face *face, const Scene *scene, double layerHeightMm,
                          const glm::dvec3 &buildDirWorld);

/// Planar face whose normal aligns with `buildDirWorld` (horizontal slice cap for default +Z build).
bool FaceIsLayerCapParallelBuild(const Face *face, const glm::dvec3 &buildDirWorld);

/// Planar face whose normal is approximately orthogonal to `buildDirWorld` (vertical wall for +Z build).
/// Uses `|dot(n̂, buildDir̂)| <= kWallNormalMaxAbsDotBuild`.
bool FaceNormalPerpendicularToBuild(const Face *face, const glm::dvec3 &buildDirWorld);

/// Wall gate: `|dot(faceNormal, buildDir̂)| <= kWallNormalMaxAbsDotBuild` (≈ layer-plane span intent).
inline constexpr double kWallNormalMaxAbsDotBuild = 0.15;

/// Nominal CAD span direction vs build axis for contour/hole must satisfy the same bound.
inline constexpr double kCalibSpanMaxAbsDotBuildAxis = 0.15;

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
