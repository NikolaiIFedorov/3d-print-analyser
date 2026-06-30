#include "LayerDifference.hpp"
#include "logic/Analysis/utils/LayerDiffUtils.hpp"
#include "GeometryOps/RingUtils.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

LayerDifference::LayerDifference(double layerHeight, double maxAreaDeltaMm2)
    : layerHeight(layerHeight), maxAreaDeltaMm2(maxAreaDeltaMm2)
{
}

std::vector<FaceFlaw> LayerDifference::Analyze(const Solid *, const std::vector<SlicedLayer> &layers) const
{
    std::vector<FaceFlaw> results;

    // Same reasoning as Overhang: compare layers ~1mm apart rather than every single sliced
    // layer, to keep the OCCT boolean call count bounded regardless of how fine layerHeight is.
    const size_t stride = std::max<size_t>(1, static_cast<size_t>(std::lround(1.0 / std::max(layerHeight, 1e-6))));

    for (size_t i = stride; i < layers.size(); i += stride)
    {
        const SlicedLayer &prev = layers[i - stride];
        const SlicedLayer &curr = layers[i];
        if (curr.loops.empty() && prev.loops.empty())
            continue;

        // The overwhelming majority of layer pairs in a real model are unchanged vertical wall
        // cross-sections — skip both expensive OCCT booleans entirely when a cheap area/centroid
        // check already confirms nothing changed.
        if (LayerDiffUtils::LayersAreEffectivelyIdentical(curr.loops, prev.loops))
            continue;

        // OCCT's boolean algorithms can take pathologically long (and a lot of memory) on
        // near-degenerate input, e.g. the tapering tip of a wedge approaching zero width. Once a
        // cross-section is already that thin, Instability/Not Enough Space's own cheap checks
        // cover it — skip the OCCT diff here rather than risk a multi-minute stall.
        if (LayerDiffUtils::AnyLoopIsDegenerate(curr.loops) || LayerDiffUtils::AnyLoopIsDegenerate(prev.loops))
            continue;

        const auto currRings = LayerDiffUtils::ToClassifiedRings(curr.loops);
        const auto prevRings = LayerDiffUtils::ToClassifiedRings(prev.loops);

        // Area changed in either direction — material gained (curr - prev) or lost
        // (prev - curr) — both indicate a sudden cross-section jump between adjacent layers.
        const auto grown = GeometryOps::RingDifference(currRings, prevRings, curr.z, 0.05);
        const auto shrunk = GeometryOps::RingDifference(prevRings, currRings, curr.z, 0.05);

        double changedAreaMm2 = 0.0;
        for (const auto &cr : grown)
            if (!cr.isHole)
                changedAreaMm2 += std::abs(GeometryOps::SignedArea2D(cr.ring, GeometryOps::kWorldUpNormal));
        for (const auto &cr : shrunk)
            if (!cr.isHole)
                changedAreaMm2 += std::abs(GeometryOps::SignedArea2D(cr.ring, GeometryOps::kWorldUpNormal));

        if (changedAreaMm2 <= maxAreaDeltaMm2)
            continue;

        // Attach each changed region's own ring as the clip boundary (like Overhang does) so
        // only the area that actually changed gets highlighted, not the whole face.
        const ZBounds zb{prev.z, curr.z};

        for (const auto &cr : grown)
        {
            if (cr.isHole || cr.ring.size() < 3)
                continue;
            std::unordered_set<const Face *> touchedFaces;
            const size_t n = cr.ring.size();
            for (size_t e = 0; e < n; e++)
            {
                const glm::dvec3 mid = 0.5 * (cr.ring[e] + cr.ring[(e + 1) % n]);
                if (const Face *f = LayerDiffUtils::NearestEdgeFace(mid, curr.loops))
                    touchedFaces.insert(f);
            }
            for (const Face *face : touchedFaces)
                results.push_back({face, FaceFlawKind::LAYER_DIFFERENCE, zb, cr.ring});
        }
        for (const auto &cr : shrunk)
        {
            if (cr.isHole || cr.ring.size() < 3)
                continue;
            std::unordered_set<const Face *> touchedFaces;
            const size_t n = cr.ring.size();
            for (size_t e = 0; e < n; e++)
            {
                const glm::dvec3 mid = 0.5 * (cr.ring[e] + cr.ring[(e + 1) % n]);
                if (const Face *f = LayerDiffUtils::NearestEdgeFace(mid, prev.loops))
                    touchedFaces.insert(f);
            }
            for (const Face *face : touchedFaces)
                results.push_back({face, FaceFlawKind::LAYER_DIFFERENCE, zb, cr.ring});
        }
    }

    return results;
}
