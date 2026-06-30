#include "OverhangDetector.hpp"
#include "logic/Analysis/utils/LayerDiffUtils.hpp"
#include "GeometryOps/ConvexHull2D.hpp"
#include "GeometryOps/RingUtils.hpp"

#include <cmath>

OverhangDetector::OverhangDetector(double layerHeight, double maxAngleDeg)
    : layerHeight(layerHeight), maxAngleDeg(maxAngleDeg), tanMaxAngle(std::tan(glm::radians(std::min(maxAngleDeg, 89.0))))
{
}

std::vector<FaceFlaw> OverhangDetector::Analyze(const Solid *, const std::vector<SlicedLayer> &layers) const
{
    std::vector<FaceFlaw> results;

    for (size_t i = 1; i < layers.size(); i++)
    {
        const SlicedLayer &prev = layers[i - 1];
        const SlicedLayer &curr = layers[i];
        if (curr.loops.empty())
            continue;

        // Treat all rings as outer (isHole = false) for the diff. isHole classification is
        // unreliable for section loops of angled faces (CW from downward-facing normals, nested
        // protrusions, etc.) and causes false positives at topology transition layers. For
        // overhang detection we only need "new solid area in curr not covered by prev" — treating
        // all rings as the union of solid outlines gives the correct diff regardless of whether
        // the model has physical bores (unchanged bores produce diff = 0 either way).
        auto currRings = LayerDiffUtils::ToClassifiedRings(curr.loops);
        auto prevRings = LayerDiffUtils::ToClassifiedRings(prev.loops);
        for (auto &r : currRings) r.isHole = false;
        for (auto &r : prevRings) r.isHole = false;
        const auto diff = GeometryOps::RingDifference(currRings, prevRings, curr.z, 0.05);

        const ZBounds zb{prev.z, curr.z};
        const double maxOffsetForSpan = (curr.z - prev.z) * tanMaxAngle;

        for (const auto &cr : diff)
        {
            if (cr.isHole || cr.ring.size() < 3)
                continue;

            const double width = GeometryOps::MinWidth2D(cr.ring, GeometryOps::kWorldUpNormal);
            if (width <= maxOffsetForSpan)
                continue;

            const auto touchedFaces = LayerDiffUtils::FacesForDiffRing(cr.ring, curr.loops, 1e-3, maxAngleDeg);

            for (const Face *face : touchedFaces)
                results.push_back({face, FaceFlawKind::OVERHANG, zb, cr.ring});
        }
    }

    return results;
}
