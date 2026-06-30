#pragma once

#include "Analysis/Analysis.hpp"

/// Per-layer-pair polygon boolean difference (currentLayer - previousLayer) gives the actual
/// unsupported region directly, robust to topology changes between layers (a hole opening, a
/// loop splitting/merging) since boolean difference needs no point-correspondence between the
/// two boundaries. Severity is scored via the unsupported region's own minimum width (Euler
/// buckling-style rotating calipers, same primitive `Instability` uses) divided by layer height,
/// which is the same overhang-angle-vs-vertical quantity the old face-normal check exposed, just
/// derived from the actual unsupported geometry instead of a single face's normal — so the
/// `overhangAngle` setting keeps its existing meaning. Driven by nozzle pressure pushing against
/// the previous layer (not gravity, which is why the same effect shows up on upside-down printers
/// too): a layer offset further outward than the nozzle can support droops/fails to bond cleanly.
class OverhangDetector : public ISolidAnalysis
{
public:
    OverhangDetector(double layerHeight = 0.2, double maxAngleDeg = 45.0);

    std::vector<FaceFlaw> Analyze(const Solid *solid, const std::vector<SlicedLayer> &layers) const override;

private:
    double layerHeight;
    double maxAngleDeg;
    double tanMaxAngle;
};
