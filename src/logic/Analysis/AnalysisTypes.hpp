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

struct Segment
{
    glm::dvec3 a;
    glm::dvec3 b;
};

struct Layer
{
    std::vector<Segment> segments;
    Flaw flaw;
    Layer(const std::vector<Segment> &segments, Flaw flaw) : segments(segments), flaw(flaw) {}
    Layer(const std::vector<Segment> &segments) : segments(segments) {};
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

struct AnalysisResults
{
    std::unordered_map<const Face *, Flaw> faceFlaws;
    std::unordered_map<const Solid *, std::vector<Layer>> solidLayers;
    std::unordered_map<const Solid *, std::vector<EdgeFlaw>> edgeFlaws;
};
