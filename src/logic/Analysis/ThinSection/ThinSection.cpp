#include "ThinSection.hpp"
#include "logic/Analysis/utils/Slice.hpp"
#include "scene/Geometry/AllGeometry.hpp"
#include <limits>

// Chain outer (non-hole) segments into an ordered polygon by greedily
// connecting the endpoint of each segment to the nearest unused segment.
// Returns polygon vertices in traversal order.
static std::vector<glm::dvec2> ChainOuter(const std::vector<Segment> &segs)
{
    if (segs.empty())
        return {};

    std::vector<bool> used(segs.size(), false);
    std::vector<glm::dvec2> polygon;
    polygon.push_back(glm::dvec2(segs[0].a));
    glm::dvec2 current = glm::dvec2(segs[0].b);
    used[0] = true;

    while (true)
    {
        double bestDist = std::numeric_limits<double>::max();
        int bestIdx = -1;
        bool bestFlipped = false;

        for (size_t i = 0; i < segs.size(); i++)
        {
            if (used[i])
                continue;
            double da = glm::length(glm::dvec2(segs[i].a) - current);
            double db = glm::length(glm::dvec2(segs[i].b) - current);
            if (da < bestDist)
            {
                bestDist = da;
                bestIdx = (int)i;
                bestFlipped = false;
            }
            if (db < bestDist)
            {
                bestDist = db;
                bestIdx = (int)i;
                bestFlipped = true;
            }
        }

        if (bestIdx == -1)
            break;

        polygon.push_back(current);
        used[bestIdx] = true;
        current = bestFlipped ? glm::dvec2(segs[bestIdx].a) : glm::dvec2(segs[bestIdx].b);
    }
    polygon.push_back(current);

    return polygon;
}

static double PolygonArea(const std::vector<glm::dvec2> &poly)
{
    double area = 0.0;
    size_t n = poly.size();
    for (size_t i = 0; i < n; i++)
    {
        size_t j = (i + 1) % n;
        area += poly[i].x * poly[j].y - poly[j].x * poly[i].y;
    }
    return std::abs(area) * 0.5;
}

static double PolygonPerimeter(const std::vector<glm::dvec2> &poly)
{
    double perim = 0.0;
    size_t n = poly.size();
    for (size_t i = 0; i < n; i++)
    {
        size_t j = (i + 1) % n;
        perim += glm::length(poly[j] - poly[i]);
    }
    return perim;
}

std::vector<FaceFlaw> ThinSection::Analyze(const Solid *solid, std::optional<ZBounds> bounds,
                                           std::vector<BridgeSurface> *bridgeSurfaces) const
{
    std::vector<FaceFlaw> results;

    auto [zMin, zMax] = bounds.value_or(Slice::GetZBounds(solid));

    if (zMax - zMin < layerHeight)
        return results;

    struct LayerInfo
    {
        double z;
        double wEff;
        std::vector<const Face *> faces;
    };

    auto layers = Slice::Range(solid, zMin, zMax, layerHeight);

    std::vector<LayerInfo> infos;
    infos.reserve(layers.size());

    for (size_t li = 0; li < layers.size(); li++)
    {
        // Reconstruct z to match Slice::Range's iteration
        double z = zMin + layerHeight * static_cast<double>(li + 1);
        const auto &layer = layers[li];

        std::vector<Segment> outerSegs;
        std::vector<const Face *> faces;
        for (const auto &seg : layer.segments)
        {
            if (!seg.isHole)
            {
                outerSegs.push_back(seg);
                if (seg.face)
                    faces.push_back(seg.face);
            }
        }

        if (outerSegs.empty())
        {
            // No cross-section at this Z — not a thin layer, breaks any run
            infos.push_back({z, std::numeric_limits<double>::max(), {}});
            continue;
        }

        auto polygon = ChainOuter(outerSegs);
        double A = PolygonArea(polygon);
        double P = PolygonPerimeter(polygon);
        // Effective width = 2A/P (hydraulic radius, not diameter).
        // For a thin rectangular wall of width w and height h >> w:
        //   2A/P = 2*w*h / (2*(w+h)) → w as h → ∞
        // This makes the threshold match the actual wall thickness 1:1.
        // (The full hydraulic diameter 4A/P would be ~2× the wall width for slabs,
        //  requiring the user to enter twice the actual thickness they want to detect.)
        double wEff = (P > 1e-10) ? 2.0 * A / P : std::numeric_limits<double>::max();

        infos.push_back({z, wEff, std::move(faces)});
    }

    // Identify contiguous runs of thin layers (wEff < minWidth)
    size_t i = 0;
    while (i < infos.size())
    {
        if (infos[i].wEff >= minWidth)
        {
            i++;
            continue;
        }

        // Start of a thin run
        size_t runStart = i;
        double minWEff = infos[i].wEff;
        std::vector<const Face *> runFaces = infos[i].faces;

        i++;
        while (i < infos.size() && infos[i].wEff < minWidth)
        {
            minWEff = std::min(minWEff, infos[i].wEff);
            for (const Face *f : infos[i].faces)
            {
                if (std::find(runFaces.begin(), runFaces.end(), f) == runFaces.end())
                    runFaces.push_back(f);
            }
            i++;
        }

        double runHeight = infos[i - 1].z - infos[runStart].z;

        // Slenderness check: run must be tall enough relative to its effective width
        if (minWEff > 1e-10 && runHeight / minWEff >= heightToWidthRatio)
        {
            ZBounds zb = {infos[runStart].z, infos[i - 1].z};
            for (const Face *face : runFaces)
                results.push_back({face, FaceFlawKind::THIN_SECTION, zb});
        }
    }

    return results;
}
