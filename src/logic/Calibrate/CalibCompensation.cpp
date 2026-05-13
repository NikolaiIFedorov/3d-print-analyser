#include "CalibCompensation.hpp"

#include <algorithm>
#include <cmath>

namespace CalibrateCompensation
{

Values Compute(CalibWorkflow workflow, float nominalMm, float measuredMm)
{
    constexpr const char kPrintMeasurement[] = "Print measurement";

    Values v;
    if (workflow == CalibWorkflow::None || nominalMm <= 1e-6f)
        return v;

    if (measuredMm <= 1e-6f)
    {
        v.errorCode = "CAL_MEASURED_ZERO";
        v.errorMessage = "Enter a positive print measurement (millimeters after unit conversion).";
        v.errorParameterLabel = kPrintMeasurement;
        return v;
    }

    switch (workflow)
    {
    case CalibWorkflow::Contour:
        v.contourScale = nominalMm / std::max(measuredMm, 1e-4f);
        v.valid = std::isfinite(v.contourScale) && v.contourScale > 1e-4f && v.contourScale < 1e4f;
        if (!v.valid)
        {
            v.errorCode = "CAL_CONTOUR_RATIO";
            v.errorMessage = "Printed span vs CAD produced an extreme scale; check the measurement and face picks.";
            v.errorParameterLabel = kPrintMeasurement;
        }
        break;
    case CalibWorkflow::Hole:
        v.holeRadiusOffsetMm = 0.5f * (nominalMm - measuredMm);
        v.valid = std::isfinite(v.holeRadiusOffsetMm);
        if (!v.valid)
        {
            v.errorCode = "CAL_NUMERIC";
            v.errorMessage = "Hole calibration produced a non-finite radius offset.";
            v.errorParameterLabel = kPrintMeasurement;
        }
        break;
    case CalibWorkflow::ElephantFoot:
        v.elephantFootExcessMm = measuredMm - nominalMm;
        v.valid = std::isfinite(v.elephantFootExcessMm);
        if (!v.valid)
        {
            v.errorCode = "CAL_NUMERIC";
            v.errorMessage = "First-layer calibration produced a non-finite excess value.";
            v.errorParameterLabel = kPrintMeasurement;
        }
        break;
    default:
        break;
    }
    return v;
}

} // namespace CalibrateCompensation
