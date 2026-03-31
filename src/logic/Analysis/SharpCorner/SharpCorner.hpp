#pragma once

#include <cmath>
#include <glm/glm.hpp>

#include "scene/Geometry/AllGeometry.hpp"
#include "Analysis/Analysis.hpp"

class SharpCorner : public ISolidAnalysis
{
public:
    SharpCorner(double angleThresholdDegrees = 40.0)
        : cosThreshold(std::cos(glm::radians(angleThresholdDegrees))) {}

    std::vector<Layer> Analyze(const Solid *solid) const override;

private:
    double cosThreshold;
};
