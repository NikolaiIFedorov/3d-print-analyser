#pragma once

#include "logic/Analysis/utils/Slice.hpp"
#include "Analysis/Analysis.hpp"

class ThinSection : public ISolidAnalysis
{
public:
    ThinSection(double layerHeight = 0.2, double widthThreshold = 1.5, double heightToWidthRatio = 3.0)
        : layerHeight(layerHeight), widthThreshold(widthThreshold), heightToWidthRatio(heightToWidthRatio) {}

    std::vector<FaceFlaw> Analyze(const Solid *solid, std::optional<ZBounds> bounds = std::nullopt) const override;

private:
    double layerHeight;
    double widthThreshold;
    double heightToWidthRatio;
};