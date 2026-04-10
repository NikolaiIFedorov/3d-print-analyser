#pragma once

#include "logic/Analysis/utils/Slice.hpp"
#include "Analysis/Analysis.hpp"

class SharpCorner;

class SmallFeature : public ISolidAnalysis
{
public:
    SmallFeature(double layerHeight = 0.2, double minFeatureSize = 1.5,
                 double heightToWidthRatio = 3.0, const SharpCorner *sharpCorner = nullptr)
        : layerHeight(layerHeight), minFeatureSize(minFeatureSize),
          heightToWidthRatio(heightToWidthRatio), sharpCorner(sharpCorner) {}

    std::vector<FaceFlaw> Analyze(const Solid *solid, std::optional<ZBounds> bounds = std::nullopt) const override;

private:
    double layerHeight;
    double minFeatureSize;
    double heightToWidthRatio;
    const SharpCorner *sharpCorner;
};
