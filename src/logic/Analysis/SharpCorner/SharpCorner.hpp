#pragma once

#include <cmath>
#include <glm/glm.hpp>

#include "scene/Geometry/AllGeometry.hpp"
#include "Analysis/Analysis.hpp"

class SharpCorner : public IEdgeAnalysis
{
public:
    SharpCorner(double angleThresholdDegrees = 100.0)
        : cosThreshold(std::cos(glm::radians(angleThresholdDegrees))) {}

    std::vector<EdgeFlaw> Analyze(const Solid *solid) const override;

private:
    double cosThreshold;
};
