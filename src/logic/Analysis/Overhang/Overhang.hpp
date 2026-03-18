#pragma once

#include <glm/glm.hpp>
#include <cmath>

#include "scene/Geometry/AllGeometry.hpp"
#include "Analysis/Analysis.hpp"

class Overhang : public IFaceAnalysis
{
public:
    std::optional<Flaw> Analyze(const Face *face) const override;
};