#pragma once

#include <vector>
#include <glm/glm.hpp>

#include "Analysis/AnalysisTypes.hpp"

struct Solid;

class Slice
{
public:
    static std::vector<Segment> At(const Solid *solid, double z);

    static std::vector<Layer> Range(const Solid *solid, double zMin, double zMax, double layerHeight);

    static double MinWidth(const std::vector<Segment> &segments);

    static ZBounds GetZBounds(const Solid *solid);

    /// Groups a layer's segments into correctly-separated closed loops (multiple disjoint
    /// islands/holes at the same Z are kept distinct, unlike the old single-polygon `ChainOuter`
    /// approach). Segments sharing an endpoint (within tolerance) are grouped via union-find,
    /// each group is walked into an ordered ring, and each ring is classified as outer or hole by
    /// point-in-polygon containment against the other rings in this layer. Degenerate groups
    /// (fewer than 3 vertices) are dropped.
    static std::vector<SliceLoop> ExtractLoops(const std::vector<Segment> &segments);
};