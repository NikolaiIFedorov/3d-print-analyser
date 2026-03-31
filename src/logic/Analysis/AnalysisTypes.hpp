#pragma once

#include <vector>
#include <glm/glm.hpp>

enum class Flaw
{
    OVERHANG,
    THIN_SECTION,
    SHARP_CORNER,
    BRIDGING,
    SMALL_FEATURE,
    NONE,
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
