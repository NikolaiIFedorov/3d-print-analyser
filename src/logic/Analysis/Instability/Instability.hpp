#pragma once

#include "Analysis/Analysis.hpp"

/// Toppling/wobble detector: per layer, per outer loop, computes the cross-section's minimum
/// width via convex hull + rotating calipers (`GeometryOps::MinWidth2D`) — the minimum extent
/// measured perpendicular to any hull edge, which is always where a convex shape's true minimum
/// width is achieved. This is the same principle as Euler buckling: a structure's resistance to
/// disturbance is governed by its weakest-direction extent, not an isotropic average — which is
/// why this replaces the old Thin Section's `2A/P` "effective width" (an isotropic measure that
/// couldn't distinguish a stable wide panel from an unstable narrow rod of the same average
/// width). Reuses Thin Section's run-detection + height/width-ratio slenderness logic essentially
/// unchanged, just fed `minWidth` instead of `2A/P`, generalized to track multiple independent
/// islands across layers (nearest-centroid matching) instead of assuming one global cross-section
/// per layer.
class Instability : public ISolidAnalysis
{
public:
    Instability(double layerHeight = 0.2, double minWidth = 2.0, double heightToWidthRatio = 3.0);

    std::vector<FaceFlaw> Analyze(const Solid *solid, const std::vector<SlicedLayer> &layers) const override;

private:
    double layerHeight;
    double minWidth;
    double heightToWidthRatio;
};
