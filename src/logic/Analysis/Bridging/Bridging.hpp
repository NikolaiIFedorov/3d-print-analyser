#pragma once

#include "scene/Geometry/AllGeometry.hpp"
#include "logic/Analysis/utils/Slice.hpp"
#include "Analysis/Analysis.hpp"

class Bridging : public ISolidAnalysis
{
public:
    Bridging(double layerHeight = 0.2, double minBridgeLength = 2.0)
        : layerHeight(layerHeight), minBridgeLength(minBridgeLength) {}

    std::vector<Layer> Analyze(const Solid *solid, std::optional<ZBounds> bounds = std::nullopt) const override;

private:
    double layerHeight;
    double minBridgeLength;
};
