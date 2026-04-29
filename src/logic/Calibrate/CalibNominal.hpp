#pragma once

class Face;

namespace CalibrateNominal
{

struct SpanResult
{
    float nominalMm = 0.0f;
    bool valid = false;
};

/// Distance between two picked faces along their shared normal direction (parallel / opposite planar
/// faces → slab thickness; otherwise centroid distance as a fallback).
SpanResult SpanBetweenFaces(const Face *a, const Face *b);

} // namespace CalibrateNominal
