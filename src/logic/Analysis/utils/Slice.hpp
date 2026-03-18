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
};