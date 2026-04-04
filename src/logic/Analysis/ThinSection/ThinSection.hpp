#pragma once

#include "scene/Geometry/AllGeometry.hpp"
#include "logic/Analysis/utils/Slice.hpp"
#include "Analysis/Analysis.hpp"

struct Layer;

class ThinSection : public ISolidAnalysis
{
public:
    ThinSection(double layerHeight = 0.2, double widthThreshold = 1.5, double aspectThreshold = 4.0)
        : layerHeight(layerHeight), widthThreshold(widthThreshold), aspectThreshold(aspectThreshold) {}

    std::vector<Layer> Analyze(const Solid *solid, std::optional<ZBounds> bounds = std::nullopt) const override;

private:
    double layerHeight;
    double widthThreshold;
    double aspectThreshold;
};