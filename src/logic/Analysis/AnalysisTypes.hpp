#pragma once

#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>

enum class FaceFlawKind
{
    OVERHANG,
    NOT_ENOUGH_SPACE,
    INSTABILITY,
    LAYER_DIFFERENCE,
    STRINGING,
    SHARP_CORNER,
    NONE,
};

struct ZBounds
{
    double zMin;
    double zMax;
};

struct Face;

struct Segment
{
    glm::dvec3 a;
    glm::dvec3 b;
    const Face *face = nullptr;
    bool isHole = false;
};

struct Triangle
{
    glm::dvec3 a, b, c;
};

/// A single closed boundary loop extracted from a layer's cross-section by `Slice::ExtractLoops`,
/// correctly separated from any other disjoint loops/islands present at the same Z.
struct SliceLoop
{
    std::vector<glm::dvec3> ring;       // ordered, closed (no duplicate closing vertex)
    std::vector<const Face *> edgeFaces; // edgeFaces[i] = face owning the edge ring[i] -> ring[(i+1)%n]
    bool isHole = false;                // true if nested inside another loop's ring
};

/// One layer's correctly-separated cross-section, shared across all detectors so each solid is
/// only sliced once per analysis pass instead of every detector re-slicing independently.
struct SlicedLayer
{
    double z = 0.0;
    std::vector<SliceLoop> loops;
};

struct Layer
{
    std::vector<Segment> segments;
    std::vector<Triangle> triangles;
    FaceFlawKind flaw;
    Layer(const std::vector<Segment> &segments, FaceFlawKind flaw) : segments(segments), flaw(flaw) {}
    Layer(const std::vector<Segment> &segments) : segments(segments) {};
    Layer(const std::vector<Triangle> &triangles, FaceFlawKind flaw) : triangles(triangles), flaw(flaw) {}
};

class Edge;
class Face;
class Solid;

struct FaceFlaw
{
    const Face *face;
    FaceFlawKind flaw;
    ZBounds bounds;
    std::vector<glm::dvec3> clipBoundary;
};

struct BridgeSurface
{
    FaceFlawKind flaw;
    std::vector<glm::dvec3> boundary; // closed polygon for a vertical connecting face
};

struct AnalysisResults
{
    std::unordered_map<const Solid *, std::vector<FaceFlaw>> faceFlawRanges;
    std::unordered_map<const Solid *, std::vector<BridgeSurface>> bridgeSurfaces;
};

class Scene;
/// Drops face/edge entries that no longer have geometry (e.g. tombstones after OCCT rebuild).
void PruneDefunctAnalysisResults(AnalysisResults &results, const Scene *scene) noexcept;
