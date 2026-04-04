#pragma once

#include "scene/Geometry/AllGeometry.hpp"
#include "logic/Analysis/utils/Slice.hpp"
#include "Analysis/Analysis.hpp"

class SmallFeature : public ISolidAnalysis
{
public:
    SmallFeature(double layerHeight = 0.2, double minFeatureSize = 0.8)
        : layerHeight(layerHeight), minFeatureSize(minFeatureSize) {}

    std::vector<Layer> Analyze(const Solid *solid, std::optional<ZBounds> bounds = std::nullopt) const override;

private:
    double layerHeight;
    double minFeatureSize;
};
