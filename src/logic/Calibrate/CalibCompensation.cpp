#include "CalibCompensation.hpp"

#include <algorithm>
#include <cmath>

namespace CalibrateCompensation
{

Values Compute(CalibWorkflow workflow, float nominalMm, float measuredMm)
{
    Values v;
    if (workflow == CalibWorkflow::None || nominalMm <= 1e-6f || measuredMm <= 1e-6f)
        return v;

    switch (workflow)
    {
    case CalibWorkflow::Contour:
        v.contourScale = nominalMm / std::max(measuredMm, 1e-4f);
        v.valid = std::isfinite(v.contourScale) && v.contourScale > 1e-4f && v.contourScale < 1e4f;
        break;
    case CalibWorkflow::Hole:
        v.holeRadiusOffsetMm = 0.5f * (nominalMm - measuredMm);
        v.valid = std::isfinite(v.holeRadiusOffsetMm);
        break;
    case CalibWorkflow::ElephantFoot:
        v.elephantFootExcessMm = measuredMm - nominalMm;
        v.valid = std::isfinite(v.elephantFootExcessMm);
        break;
    default:
        break;
    }
    return v;
}

} // namespace CalibrateCompensation
