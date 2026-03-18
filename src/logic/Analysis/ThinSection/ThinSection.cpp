#include "ThinSection.hpp"
#include "logic/Analysis/utils/Slice.hpp"
#include <limits>

std::vector<Layer> ThinSection::Analyze(const Solid *solid) const
{
    std::vector<Layer> thinLayers;

    double zMin = std::numeric_limits<double>::max();
    double zMax = std::numeric_limits<double>::lowest();

    for (const Face *face : solid->faces)
    {
        for (const auto &loop : face->loops)
        {
            for (const auto &orientedEdge : loop)
            {
                const Edge *edge = orientedEdge.edge;
                zMin = std::min(zMin, edge->startPoint->position.z);
                zMax = std::max(zMax, edge->startPoint->position.z);
                zMin = std::min(zMin, edge->endPoint->position.z);
                zMax = std::max(zMax, edge->endPoint->position.z);
            }
        }
    }

    if (zMax - zMin < layerHeight)
        return thinLayers;

    auto layers = Slice::Range(solid, zMin, zMax, layerHeight);

    std::vector<Layer> currentRun;
    double worstWidth = std::numeric_limits<double>::max();

    for (const auto &layer : layers)
    {
        if (layer.segments.empty())
        {
            if (!currentRun.empty())
            {
                double thinHeight = currentRun.size() * layerHeight;
                if (thinHeight / worstWidth >= aspectThreshold)
                {
                    for (auto &l : currentRun)
                        l.flaw = Flaw::THIN_SECTION;
                    thinLayers.insert(thinLayers.end(), currentRun.begin(), currentRun.end());
                }

                currentRun.clear();
                worstWidth = std::numeric_limits<double>::max();
            }
            continue;
        }

        double width = Slice::MinWidth(layer.segments);

        if (width < widthThreshold)
        {
            currentRun.push_back(layer);
            worstWidth = std::min(worstWidth, width);
        }
        else
        {
            if (!currentRun.empty())
            {
                double thinHeight = currentRun.size() * layerHeight;
                if (thinHeight / worstWidth >= aspectThreshold)
                {
                    for (auto &l : currentRun)
                        l.flaw = Flaw::THIN_SECTION;
                    thinLayers.insert(thinLayers.end(), currentRun.begin(), currentRun.end());
                }

                currentRun.clear();
                worstWidth = std::numeric_limits<double>::max();
            }
        }
    }

    if (!currentRun.empty())
    {
        double thinHeight = currentRun.size() * layerHeight;
        if (thinHeight / worstWidth >= aspectThreshold)
        {
            for (auto &l : currentRun)
                l.flaw = Flaw::THIN_SECTION;
            thinLayers.insert(thinLayers.end(), currentRun.begin(), currentRun.end());
        }
    }

    return thinLayers;
}