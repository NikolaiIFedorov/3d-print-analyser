#include "SmallFeature.hpp"
#include "logic/Analysis/utils/Slice.hpp"

std::vector<Layer> SmallFeature::Analyze(const Solid *solid, std::optional<ZBounds> bounds) const
{
    std::vector<Layer> smallLayers;

    auto [zMin, zMax] = bounds.value_or(Slice::GetZBounds(solid));

    if (zMax - zMin < layerHeight)
        return smallLayers;

    auto layers = Slice::Range(solid, zMin, zMax, layerHeight);

    for (const auto &layer : layers)
    {
        if (layer.segments.empty())
            continue;

        // Check for segments that are individually too small
        std::vector<Segment> smallSegments;
        for (const auto &seg : layer.segments)
        {
            double len = glm::length(seg.b - seg.a);
            if (len < minFeatureSize && len > 1e-10)
            {
                smallSegments.push_back(seg);
            }
        }

        // Also check if the minimum width across the cross-section
        // is below the nozzle-printable threshold
        if (layer.segments.size() >= 2)
        {
            double minWidth = Slice::MinWidth(layer.segments);
            if (minWidth < minFeatureSize)
            {
                // Flag all segments in this layer as small features
                smallLayers.emplace_back(layer.segments, Flaw::SMALL_FEATURE);
                continue;
            }
        }

        if (!smallSegments.empty())
        {
            smallLayers.emplace_back(smallSegments, Flaw::SMALL_FEATURE);
        }
    }

    return smallLayers;
}
