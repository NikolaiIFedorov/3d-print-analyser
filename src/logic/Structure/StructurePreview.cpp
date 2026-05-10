#include "Structure/StructurePreview.hpp"

#include "scene/scene.hpp"
#include "Geometry/Edge.hpp"
#include "Geometry/Face.hpp"
#include "Geometry/Solid.hpp"
#include "Geometry/Surface.hpp"

#include <cmath>
#include <limits>

namespace StructurePreview
{
namespace
{
void ExpandBounds(const glm::dvec3 &p, glm::dvec3 &mn, glm::dvec3 &mx)
{
    mn = glm::min(mn, p);
    mx = glm::max(mx, p);
}

bool SolidBounds(const Solid &solid, glm::dvec3 &outMin, glm::dvec3 &outMax)
{
    glm::dvec3 mn(std::numeric_limits<double>::infinity());
    glm::dvec3 mx(-std::numeric_limits<double>::infinity());
    bool any = false;
    for (const Face *fp : solid.faces)
    {
        if (fp == nullptr)
            continue;
        const Face &face = *fp;
        if (face.loops.empty() || face.loops[0].empty())
            continue;
        for (const OrientedEdge &oe : face.loops[0])
        {
            if (oe.edge == nullptr)
                continue;
            ExpandBounds(oe.GetStartPosition(), mn, mx);
            any = true;
        }
    }
    if (!any)
        return false;
    outMin = mn;
    outMax = mx;
    return true;
}

glm::dvec3 FaceCentroidPlanar(const Face &face)
{
    if (face.loops.empty() || face.loops[0].empty())
        return glm::dvec3(0.0);
    glm::dvec3 sum(0.0);
    std::size_t n = 0;
    for (const OrientedEdge &oe : face.loops[0])
    {
        if (oe.edge == nullptr)
            continue;
        sum += oe.GetStartPosition();
        ++n;
    }
    if (n == 0)
        return glm::dvec3(0.0);
    return sum / static_cast<double>(n);
}

} // namespace

void BuildAdjacentFaceMidpoints(const Scene &scene, std::vector<std::pair<glm::vec3, glm::vec3>> &out)
{
    out.clear();
    constexpr double kMinSegMm = 1.0e-4;
    constexpr double kEps2 = 1.0e-18;

    for (const Edge &edge : scene.edges)
    {
        if (edge.dependencies.size() != 2)
            continue;

        Face *fa = nullptr;
        Face *fb = nullptr;
        for (Face *fp : edge.dependencies)
        {
            if (fa == nullptr)
                fa = fp;
            else if (fb == nullptr && fp != fa)
                fb = fp;
        }
        if (fa == nullptr || fb == nullptr)
            continue;

        if (!fa->dependency || fa->dependency != fb->dependency)
            continue;

        if (!fa->GetSurface().IsPlanar() || !fb->GetSurface().IsPlanar())
            continue;

        const glm::dvec3 ca = FaceCentroidPlanar(*fa);
        const glm::dvec3 cb = FaceCentroidPlanar(*fb);
        const glm::dvec3 d = cb - ca;
        if (glm::dot(d, d) < kEps2)
            continue;
        if (glm::length(d) < kMinSegMm)
            continue;

        out.emplace_back(glm::vec3(ca), glm::vec3(cb));
    }
}

void BuildCenterStruts(const Scene &scene, std::vector<std::pair<glm::vec3, glm::vec3>> &out)
{
    out.clear();
    constexpr double kAlongToCenter = 0.48;
    constexpr double kDiagCap = 0.22;
    constexpr double kMinStrutMm = 0.35;
    constexpr double kEps = 1e-9;

    for (const Solid &solid : scene.solids)
    {
        glm::dvec3 bmin, bmax;
        if (!SolidBounds(solid, bmin, bmax))
            continue;
        const glm::dvec3 center = (bmin + bmax) * 0.5;
        const double diag = glm::length(bmax - bmin);
        if (!(diag > kEps))
            continue;
        const double maxLen = kDiagCap * diag;

        for (const Face *fp : solid.faces)
        {
            if (fp == nullptr || fp->dependency != &solid)
                continue;
            const Face &face = *fp;
            if (!face.GetSurface().IsPlanar())
                continue;
            const glm::dvec3 centroid = FaceCentroidPlanar(face);
            glm::dvec3 toCenter = center - centroid;
            const double len = glm::length(toCenter);
            if (!(len > kEps))
                continue;
            const glm::dvec3 dir = toCenter / len;
            double strutLen = std::min(kAlongToCenter * len, maxLen);
            if (strutLen < kMinStrutMm)
                continue;
            const glm::dvec3 a = centroid;
            const glm::dvec3 b = centroid + dir * strutLen;
            out.emplace_back(glm::vec3(a), glm::vec3(b));
        }
    }
}

} // namespace StructurePreview
