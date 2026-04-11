#pragma once

#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>

enum class FaceFlawKind
{
    OVERHANG,
    THIN_SECTION,
    SMALL_FEATURE,
    NONE,
};

enum class EdgeFlawKind
{
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

struct EdgeFlaw
{
    const Edge *edge;
    EdgeFlawKind flaw;
    ZBounds bounds;
};

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
    std::unordered_map<const Face *, FaceFlawKind> faceFlaws;
    std::unordered_map<const Solid *, std::vector<FaceFlaw>> faceFlawRanges;
    std::unordered_map<const Solid *, std::vector<EdgeFlaw>> edgeFlaws;
    std::unordered_map<const Solid *, std::vector<BridgeSurface>> bridgeSurfaces;
};
