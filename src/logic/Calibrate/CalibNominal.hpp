#pragma once

#include <glm/glm.hpp>

class Face;
struct Edge;

namespace CalibrateNominal
{

/// Minimum |dot(na, nb)| for treating two planar picks as a parallel pair (slab thickness path).
inline constexpr double kFaceNormalParallelAlignThreshold = 0.75;

struct SpanResult
{
    float nominalMm = 0.0f;
    bool valid = false;
};

/// Segment endpoints matching `SpanBetweenFaces` / nominal slab logic (for viewport preview).
struct SpanPreview
{
    float nominalMm = 0.0f;
    bool valid = false;
    glm::dvec3 p0{};
    glm::dvec3 p1{};
};

/// True if both faces exist, are distinct, and normals are parallel or opposite within
/// `kFaceNormalParallelAlignThreshold` (used to constrain Calibrate second pick).
bool NormalsAlignedForCalibPick(const Face *a, const Face *b);

/// Loop-vertex centroid of face boundaries (same anchor used inside span preview).
glm::dvec3 FaceCentroidWorld(const Face *f);

SpanPreview SpanPreviewBetweenFaces(const Face *a, const Face *b);

/// Distance between two picked faces along their shared normal direction (parallel / opposite planar
/// faces → slab thickness; otherwise centroid distance as a fallback).
SpanResult SpanBetweenFaces(const Face *a, const Face *b);

bool EdgeBelongsToFace(const Edge *e, const Face *f);

/// Straight-edge chord parallelism (|dot| ≥ 0.985); curved edges use start→end chord.
bool EdgesAreParallelForCalib(const Edge *a, const Edge *b);

SpanPreview SpanPreviewBetweenParallelEdgesOnFace(const Face *face, const Edge *eA, const Edge *eB);

SpanResult SpanBetweenParallelEdgesOnFace(const Face *face, const Edge *eA, const Edge *eB);

/// True when `SpanPreviewBetweenFaces` segment direction is ≈ orthogonal to build (layer-plane span).
bool NominalSpanPerpendicularToBuild(const Face *a, const Face *b, const glm::dvec3 &buildDirWorld);

} // namespace CalibrateNominal
