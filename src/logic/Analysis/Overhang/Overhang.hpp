#pragma once

#include "scene/Geometry/AllGeometry.hpp"
#include "logic/Analysis/utils/Slice.hpp"
#include "Analysis/Analysis.hpp"

class Overhang : public ISolidAnalysis
{
public:
    Overhang(double layerHeight = 0.2)
        : layerHeight(layerHeight) {}

    std::vector<Layer> Analyze(const Solid *solid, std::optional<ZBounds> bounds = std::nullopt) const override;

private:
    double layerHeight;
};