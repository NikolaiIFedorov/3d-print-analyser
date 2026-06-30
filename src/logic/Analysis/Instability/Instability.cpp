#include "Instability.hpp"
#include "GeometryOps/ConvexHull2D.hpp"
#include "GeometryOps/RingUtils.hpp"

#include <limits>
#include <unordered_set>

namespace
{
    struct OpenRun
    {
        double startZ = 0.0;
        double lastZ = 0.0;
        double minWidthSoFar = std::numeric_limits<double>::max();
        glm::dvec2 lastCentroid{0.0};
        std::unordered_set<const Face *> faces;
    };
} // namespace

Instability::Instability(double layerHeight, double minWidth, double heightToWidthRatio)
    : layerHeight(layerHeight), minWidth(minWidth), heightToWidthRatio(heightToWidthRatio)
{
}

std::vector<FaceFlaw> Instability::Analyze(const Solid *, const std::vector<SlicedLayer> &layers) const
{
    std::vector<FaceFlaw> results;
    std::vector<OpenRun> openRuns;

    auto closeRun = [&](const OpenRun &run)
    {
        const double runHeight = run.lastZ - run.startZ;
        if (run.minWidthSoFar > 1e-10 && runHeight / run.minWidthSoFar >= heightToWidthRatio)
        {
            const ZBounds zb{run.startZ, run.lastZ};
            for (const Face *face : run.faces)
                results.push_back({face, FaceFlawKind::INSTABILITY, zb});
        }
    };

    for (const auto &layer : layers)
    {
        std::vector<bool> runMatched(openRuns.size(), false);
        std::vector<OpenRun> nextOpenRuns;

        for (const auto &loop : layer.loops)
        {
            if (loop.isHole || loop.ring.size() < 3)
                continue;

            const double width = GeometryOps::MinWidth2D(loop.ring, GeometryOps::kWorldUpNormal);
            const glm::dvec2 centroid = GeometryOps::RingCentroidXy(loop.ring);

            // Match to the nearest still-open run (continuing the same island across layers);
            // no correspondence is assumed beyond proximity — islands splitting/merging between
            // layers are an accepted approximation, not solved exactly here.
            int bestIdx = -1;
            double bestDist = std::numeric_limits<double>::max();
            for (size_t r = 0; r < openRuns.size(); r++)
            {
                if (runMatched[r])
                    continue;
                const double d = glm::length(centroid - openRuns[r].lastCentroid);
                if (d < bestDist)
                {
                    bestDist = d;
                    bestIdx = static_cast<int>(r);
                }
            }

            if (width >= minWidth)
            {
                // Thick enough now — close out whatever run this island was continuing, if any.
                if (bestIdx >= 0)
                {
                    runMatched[bestIdx] = true;
                    closeRun(openRuns[static_cast<size_t>(bestIdx)]);
                }
                continue;
            }

            OpenRun run;
            if (bestIdx >= 0)
            {
                runMatched[bestIdx] = true;
                run = openRuns[static_cast<size_t>(bestIdx)];
                run.minWidthSoFar = std::min(run.minWidthSoFar, width);
            }
            else
            {
                run.startZ = layer.z;
                run.minWidthSoFar = width;
            }
            run.lastZ = layer.z;
            run.lastCentroid = centroid;
            for (const Face *f : loop.edgeFaces)
                if (f)
                    run.faces.insert(f);
            nextOpenRuns.push_back(std::move(run));
        }

        // Any run not matched by a loop in this layer has ended (island vanished or merged away).
        for (size_t r = 0; r < openRuns.size(); r++)
            if (!runMatched[r])
                closeRun(openRuns[r]);

        openRuns = std::move(nextOpenRuns);
    }

    for (const auto &run : openRuns)
        closeRun(run);

    return results;
}
