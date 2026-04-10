#pragma once

#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>

enum class Flaw
{
    OVERHANG,
    THIN_SECTION,
    SHARP_CORNER,
    SMALL_FEATURE,
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
};

struct Triangle
{
    glm::dvec3 a, b, c;
};

struct Layer
{
    std::vector<Segment> segments;
    std::vector<Triangle> triangles;
    Flaw flaw;
    Layer(const std::vector<Segment> &segments, Flaw flaw) : segments(segments), flaw(flaw) {}
    Layer(const std::vector<Segment> &segments) : segments(segments) {};
    Layer(const std::vector<Triangle> &triangles, Flaw flaw) : triangles(triangles), flaw(flaw) {}
};

class Edge;
class Face;
class Solid;

struct EdgeFlaw
{
    const Edge *edge;
    Flaw flaw;
    ZBounds bounds;
};

struct FaceFlaw
{
    const Face *face;
    Flaw flaw;
    ZBounds bounds;
    std::vector<glm::dvec3> clipBoundary;
};

struct AnalysisResults
{
    std::unordered_map<const Face *, Flaw> faceFlaws;
    std::unordered_map<const Solid *, std::vector<FaceFlaw>> faceFlawRanges;
    std::unordered_map<const Solid *, std::vector<EdgeFlaw>> edgeFlaws;
};
