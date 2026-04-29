#pragma once

#include "logic/Analysis/utils/Slice.hpp"
#include "Analysis/Analysis.hpp"

class ThinSection : public ISolidAnalysis
{
public:
    ThinSection(double layerHeight = 0.2, double minWidth = 2.0, double heightToWidthRatio = 3.0)
        : layerHeight(layerHeight), minWidth(minWidth), heightToWidthRatio(heightToWidthRatio) {}

    std::vector<FaceFlaw> Analyze(const Solid *solid, std::optional<ZBounds> bounds = std::nullopt,
                                  std::vector<BridgeSurface> *bridgeSurfaces = nullptr) const override;

private:
    double layerHeight;
    double minWidth;
    double heightToWidthRatio;
};