#pragma once

#include "Analysis/Analysis.hpp"

class Overhang : public IFaceAnalysis
{
public:
    Overhang(double maxAngleDeg = 45.0);

    std::optional<FaceFlawKind> Analyze(const Face *face) const override;

private:
    double minZComponent;
};