#pragma once

#include "Analysis/Analysis.hpp"

/// Warping risk detector: shares Overhang's per-layer-pair polygon boolean difference primitive,
/// but scores the *magnitude* of cross-section change between adjacent layers (in either
/// direction — area gained or lost) rather than a directional unsupported offset. A large jump in
/// cross-sectional area between adjacent layers means very different amounts of material cooled
/// at that height, which is the actual physical driver of warping (differential cooling/
/// shrinkage), not "uneven heating" at the nozzle itself.
class LayerDifference : public ISolidAnalysis
{
public:
    LayerDifference(double layerHeight = 0.2, double maxAreaDeltaMm2 = 50.0);

    std::vector<FaceFlaw> Analyze(const Solid *solid, const std::vector<SlicedLayer> &layers) const override;

private:
    double layerHeight;
    double maxAreaDeltaMm2;
};
