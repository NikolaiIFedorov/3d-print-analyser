#pragma once

#include <unordered_set>

struct Edge;
class Face;
struct Point;
class Scene;

/// Shown in the Calibrate tool; matches product language (contour ≈ outer shrink / dimension).
enum class CalibWorkflow
{
    None,
    Contour,
    Hole,
    ElephantFoot
};

namespace CalibrateDistance
{

/// Edges that lie on inner (hole) loops of any planar face — the "hole vector" for edge tests.
void RebuildHoleEdgeSet(const Scene &scene, std::unordered_set<const Edge *> &out);

/// Builds `holeEdges` plus endpoints of those edges (`holeRingPoints`) so vertical / side faces that share
/// hole-ring vertices (but not necessarily the same `Edge*` as the planar inner loop) still classify as hole.
void RebuildHoleCalibTopology(const Scene &scene, std::unordered_set<const Edge *> &holeEdgesOut,
                             std::unordered_set<const Point *> &holeRingPointsOut);

bool FaceInFirstLayerSlab(const Face *face, const Scene *scene, double layerHeightMm);

/// Hole: multi-loop planar patch (outer+inner), boundary uses a hole inner-loop edge, or uses a hole-ring vertex.
bool FaceQualifiesAsHole(const Face *face, const std::unordered_set<const Edge *> &holeEdges,
                         const std::unordered_set<const Point *> &holeRingPoints);

CalibWorkflow ClassifyFace(const Face *face, const Scene *scene, double layerHeightMm,
                           const std::unordered_set<const Edge *> &holeEdges,
                           const std::unordered_set<const Point *> &holeRingPoints);

/// True unless the pair mixes contour vs hole (outer shrink vs opening — different compensation paths).
inline bool CalibSecondPickWorkflowsCompatible(CalibWorkflow a, CalibWorkflow b)
{
    return !((a == CalibWorkflow::Contour && b == CalibWorkflow::Hole) ||
             (a == CalibWorkflow::Hole && b == CalibWorkflow::Contour));
}

/// After two picks: elephant if either is first-layer; else hole if either qualifies; else contour.
CalibWorkflow CombinePickedFaces(const Face *a, const Face *b, const Scene *scene, double layerHeightMm,
                                 const std::unordered_set<const Edge *> &holeEdges,
                                 const std::unordered_set<const Point *> &holeRingPoints);

} // namespace CalibrateDistance
