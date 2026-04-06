#include "Overhang.hpp"
#include "logic/Analysis/utils/Slice.hpp"

static bool PointInsideContour(const glm::dvec2 &p, const std::vector<Segment> &segments)
{
    int crossings = 0;
    for (const auto &seg : segments)
    {
        glm::dvec2 a(seg.a.x, seg.a.y);
        glm::dvec2 b(seg.b.x, seg.b.y);

        if ((a.y <= p.y && b.y > p.y) || (b.y <= p.y && a.y > p.y))
        {
            double t = (p.y - a.y) / (b.y - a.y);
            if (p.x < a.x + t * (b.x - a.x))
                crossings++;
        }
    }
    return (crossings % 2) == 1;
}

std::vector<Layer> Overhang::Analyze(const Solid *solid, std::optional<ZBounds> bounds) const
{
    std::vector<Layer> overhangLayers;

    auto [zMin, zMax] = bounds.value_or(Slice::GetZBounds(solid));

    if (zMax - zMin < layerHeight * 2)
        return overhangLayers;

    auto layers = Slice::Range(solid, zMin, zMax, layerHeight);

    for (size_t i = 1; i < layers.size(); i++)
    {
        const auto &current = layers[i].segments;
        const auto &below = layers[i - 1].segments;

        if (current.empty() || below.empty())
            continue;

        std::vector<Segment> unsupported;

        for (const auto &seg : current)
        {
            glm::dvec3 mid = (seg.a + seg.b) * 0.5;

            if (!PointInsideContour(glm::dvec2(mid.x, mid.y), below))
            {
                unsupported.push_back(seg);
            }
        }

        if (!unsupported.empty())
        {
            overhangLayers.emplace_back(unsupported, Flaw::OVERHANG);
        }
    }

    return overhangLayers;
}