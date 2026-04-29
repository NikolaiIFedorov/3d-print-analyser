#pragma once

#include <unordered_set>

class Edge;
class Face;
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

bool FaceInFirstLayerSlab(const Face *face, const Scene *scene, double layerHeightMm);

/// Hole if the face has ≥ two inner loops, or any of its edges appear in `holeEdges`.
bool FaceQualifiesAsHole(const Face *face, const std::unordered_set<const Edge *> &holeEdges);

CalibWorkflow ClassifyFace(const Face *face, const Scene *scene, double layerHeightMm,
                           const std::unordered_set<const Edge *> &holeEdges);

/// After two picks: elephant if either is first-layer; else hole if either qualifies; else contour.
CalibWorkflow CombinePickedFaces(const Face *a, const Face *b, const Scene *scene, double layerHeightMm,
                                 const std::unordered_set<const Edge *> &holeEdges);

} // namespace CalibrateDistance
