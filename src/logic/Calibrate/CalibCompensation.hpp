#pragma once

#include "CalibDistanceType.hpp"

namespace CalibrateCompensation
{

/// Contour: XY scale factor (CAD / printed) when the printed outer span is smaller than nominal.
/// Hole: suggested hole radius offset in mm (half of diameter error).
/// Elephant's foot: excess linear width vs nominal (printed − CAD) in mm.
struct Values
{
    float contourScale = 1.0f;
    float holeRadiusOffsetMm = 0.0f;
    float elephantFootExcessMm = 0.0f;
    bool valid = false;
};

Values Compute(CalibWorkflow workflow, float nominalMm, float measuredMm);

} // namespace CalibrateCompensation
