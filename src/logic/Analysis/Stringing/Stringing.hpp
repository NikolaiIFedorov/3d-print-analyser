#pragma once

#include "logic/Analysis/utils/Slice.hpp"
#include "Analysis/Analysis.hpp"

class Stringing : public ISolidAnalysis
{
public:
    explicit Stringing(double layerHeight = 0.2, int maxContourCount = 1)
        : layerHeight(layerHeight), maxContourCount(maxContourCount) {}

    std::vector<FaceFlaw> Analyze(const Solid *solid, std::optional<ZBounds> bounds = std::nullopt,
                                  std::vector<BridgeSurface> *bridgeSurfaces = nullptr) const override;

private:
    double layerHeight;
    int maxContourCount; // layers with more distinct contour loops than this are flagged
};
