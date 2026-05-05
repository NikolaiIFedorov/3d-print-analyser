#pragma once

#include <glm/glm.hpp>

class Face;
class Scene;

/// Shown in the Calibrate tool.
/// `Hole` = **layer hole**: annular (or multi-loop) **cap** face parallel to the build direction — not
/// faces that merely touch hole boundary (which would mis-label contour faces).
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

/// Face lies within the first slice of thickness `layerHeightMm` along `buildDirWorld` from the scene extent.
bool FaceInFirstLayerSlab(const Face *face, const Scene *scene, double layerHeightMm,
                          const glm::dvec3 &buildDirWorld);

/// True if the face is a planar cap with ≥2 boundary loops (outer + at least one inner) and ∥ build direction.
bool FaceQualifiesAsHole(const Face *face, const glm::dvec3 &buildDirWorld);

CalibWorkflow ClassifyFace(const Face *face, const Scene *scene, double layerHeightMm,
                           const glm::dvec3 &buildDirWorld);

/// True unless the pair mixes contour vs layer-hole workflow.
inline bool CalibSecondPickWorkflowsCompatible(CalibWorkflow a, CalibWorkflow b)
{
    return !((a == CalibWorkflow::Contour && b == CalibWorkflow::Hole) ||
             (a == CalibWorkflow::Hole && b == CalibWorkflow::Contour));
}

CalibWorkflow CombinePickedFaces(const Face *a, const Face *b, const Scene *scene, double layerHeightMm,
                                   const glm::dvec3 &buildDirWorld);

} // namespace CalibrateDistance
