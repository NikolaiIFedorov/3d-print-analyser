#pragma once

#include "Analysis/Analysis.hpp"

/// Flags sharp corners where printer motion causes bulging: the nozzle cannot change direction
/// instantly, so tight turns produce corner overshoot. Each flagged slice corner is mapped back
/// to the 3D edge shared between its two adjacent faces; detections are grouped by Edge* across
/// layers to build a Z range. Horizontal edges (layer boundaries) are excluded. The edge
/// endpoints are stored as a 2-point clipBoundary for the line renderer.
class SharpCorner : public ISolidAnalysis
{
public:
    SharpCorner(double layerHeight = 0.2, double thresholdDegrees = 54.0);

    std::vector<FaceFlaw> Analyze(const Solid *solid, const std::vector<SlicedLayer> &layers) const override;

private:
    double layerHeight;
    double thresholdRadians;
};
