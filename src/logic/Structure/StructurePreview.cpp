#include "Structure/StructurePreview.hpp"

#include "scene/scene.hpp"
#include "Geometry/Edge.hpp"
#include "Geometry/Face.hpp"
#include "Geometry/Solid.hpp"
#include "Geometry/Surface.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

// Phase A of the structure pivot (see documentation/implementations/structure_face_triangulation_2026-05-11.md)
// removed the retired infill generators (BuildAdjacentFaceMidpoints / BuildInteriorFaceRibs /
// BuildInsetFaceLoops / BuildCenterStruts). The anonymous-namespace helpers below are kept and tagged
// `[[maybe_unused]]` until Phase B's face-triangulation algorithm reintroduces callers (planar face
// filter, AABB rays, in-plane (u,v) framing, polygon clipping, rib-style rectangle emission).

namespace StructurePreview
{
namespace
{
[[maybe_unused]] void ExpandBounds(const glm::dvec3 &p, glm::dvec3 &mn, glm::dvec3 &mx)
{
    mn = glm::min(mn, p);
    mx = glm::max(mx, p);
}

[[maybe_unused]] bool SolidBounds(const Solid &solid, glm::dvec3 &outMin, glm::dvec3 &outMax)
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

[[maybe_unused]] glm::dvec3 FaceCentroidPlanar(const Face &face)
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

[[maybe_unused, nodiscard]] glm::dvec3 OutwardNormalPlanar(const Face &face)
{
    const auto *planar = dynamic_cast<const PlanarSurface *>(&face.GetSurface());
    if (planar == nullptr)
        return glm::dvec3(0.0, 0.0, 1.0);
    glm::dvec3 n = planar->data.normal;
    const double ln = glm::length(n);
    if (!(ln > 1e-30))
        return glm::dvec3(0.0, 0.0, 1.0);
    return n / ln;
}

[[maybe_unused]] void MergeCloseSorted(std::vector<double> &vals, double eps)
{
    std::sort(vals.begin(), vals.end());
    std::vector<double> merged;
    merged.reserve(vals.size());
    for (double v : vals)
    {
        if (merged.empty() || std::abs(v - merged.back()) > eps)
            merged.push_back(v);
    }
    vals.swap(merged);
}

/// Line su = `uConst` in polygon (su, sv). Returns sv span [outLow, outHigh] (−Z style order irrelevant).
[[maybe_unused, nodiscard]] bool ClipConvexPolygonVerticalLine(double uConst, const std::vector<glm::dvec2> &poly,
                                                                double eps, double &outSvLow, double &outSvHigh)
{
    std::vector<double> svHits;
    const int n = static_cast<int>(poly.size());
    if (n < 3)
        return false;

    for (int i = 0; i < n; ++i)
    {
        const glm::dvec2 &a = poly[i];
        const glm::dvec2 &b = poly[(i + 1) % n];
        const double dex = b.x - a.x;

        if (std::abs(dex) < eps)
        {
            if (std::abs(a.x - uConst) < eps)
            {
                svHits.push_back(a.y);
                svHits.push_back(b.y);
            }
            continue;
        }

        const double t = (uConst - a.x) / dex;
        if (t >= -eps && t <= 1.0 + eps)
        {
            const double sv = a.y + t * (b.y - a.y);
            svHits.push_back(sv);
        }
    }

    MergeCloseSorted(svHits, std::max(1e-7, eps * 10.0));
    if (svHits.size() < 2)
        return false;
    outSvLow = svHits.front();
    outSvHigh = svHits.back();
    return outSvHigh - outSvLow > eps;
}

/// Line sv = `vConst`; returns su span.
[[maybe_unused, nodiscard]] bool ClipConvexPolygonHorizontalLine(double vConst, const std::vector<glm::dvec2> &poly,
                                                                  double eps, double &outSuLow, double &outSuHigh)
{
    std::vector<double> suHits;
    const int n = static_cast<int>(poly.size());
    if (n < 3)
        return false;

    for (int i = 0; i < n; ++i)
    {
        const glm::dvec2 &a = poly[i];
        const glm::dvec2 &b = poly[(i + 1) % n];
        const double dey = b.y - a.y;

        if (std::abs(dey) < eps)
        {
            if (std::abs(a.y - vConst) < eps)
            {
                suHits.push_back(a.x);
                suHits.push_back(b.x);
            }
            continue;
        }

        const double t = (vConst - a.y) / dey;
        if (t >= -eps && t <= 1.0 + eps)
        {
            const double su = a.x + t * (b.x - a.x);
            suHits.push_back(su);
        }
    }

    MergeCloseSorted(suHits, std::max(1e-7, eps * 10.0));
    if (suHits.size() < 2)
        return false;
    outSuLow = suHits.front();
    outSuHigh = suHits.back();
    return outSuHigh - outSuLow > eps;
}

/// Move endpoints inward along the chord; returns false if the segment collapses.
[[maybe_unused, nodiscard]] bool ApplyChordEndInset(glm::dvec3 &a0, glm::dvec3 &a1, double insetPerEndMm,
                                                    double minRemainingLengthMm)
{
    if (insetPerEndMm <= 1.0e-9)
        return true;

    const glm::dvec3 chord = a1 - a0;
    const double fullLen = glm::length(chord);
    if (!(fullLen > 1.0e-9))
        return false;
    const glm::dvec3 dir = chord / fullLen;
    const double trim = 2.0 * insetPerEndMm;
    if (!(fullLen > trim + minRemainingLengthMm))
        return false;
    a0 = a0 + dir * insetPerEndMm;
    a1 = a1 - dir * insetPerEndMm;
    return true;
}

/// Chord lies too close to the XY plane (unsupportable / “horizontal” in FDM with build-up +Z).
[[maybe_unused, nodiscard]] bool ChordTooHorizontalForBuildUp(const glm::dvec3 &a0, const glm::dvec3 &a1,
                                                               const glm::dvec3 &buildUpUnitZ, double minAbsDotZ)
{
    const glm::dvec3 chord = a1 - a0;
    const double len = glm::length(chord);
    if (!(len > 1.0e-9))
        return true;
    const glm::dvec3 dir = chord / len;
    return std::fabs(glm::dot(dir, buildUpUnitZ)) < minAbsDotZ;
}

[[maybe_unused, nodiscard]] bool RayAabbInterval(const glm::dvec3 &ro, const glm::dvec3 &rd, const glm::dvec3 &bmin,
                                                  const glm::dvec3 &bmax, double &tEnter, double &tExit)
{
    double tNear = -std::numeric_limits<double>::infinity();
    double tFar = std::numeric_limits<double>::infinity();
    for (int ax = 0; ax < 3; ++ax)
    {
        if (std::abs(rd[ax]) < 1e-15)
        {
            if (ro[ax] < bmin[ax] || ro[ax] > bmax[ax])
                return false;
            continue;
        }
        const double inv = 1.0 / rd[ax];
        double t1 = (bmin[ax] - ro[ax]) * inv;
        double t2 = (bmax[ax] - ro[ax]) * inv;
        double tNearSlab = std::min(t1, t2);
        double tFarSlab = std::max(t1, t2);
        tNear = std::max(tNear, tNearSlab);
        tFar = std::min(tFar, tFarSlab);
        if (tNear > tFar)
            return false;
    }
    tEnter = tNear;
    tExit = tFar;
    return true;
}

[[maybe_unused]] void AppendRibRectangle(std::vector<std::pair<glm::vec3, glm::vec3>> &out, const glm::dvec3 &a0,
                                          const glm::dvec3 &a1, const glm::dvec3 &inwardUnit, double depthMm)
{
    constexpr double kMinChord = 5.0e-4;
    if (glm::length(a1 - a0) < kMinChord)
        return;

    if (depthMm <= 1.0e-6)
    {
        out.emplace_back(glm::vec3(a0), glm::vec3(a1));
        return;
    }

    const glm::dvec3 b0 = a0 + inwardUnit * depthMm;
    const glm::dvec3 b1 = a1 + inwardUnit * depthMm;

    const glm::vec3 av0(a0);
    const glm::vec3 av1(a1);
    const glm::vec3 bv0(b0);
    const glm::vec3 bv1(b1);

    out.emplace_back(av0, av1);
    out.emplace_back(bv0, bv1);
    out.emplace_back(av0, bv0);
    out.emplace_back(av1, bv1);
}

} // namespace
} // namespace StructurePreview
