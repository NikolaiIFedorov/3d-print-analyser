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

[[nodiscard]] glm::dvec3 OutwardNormalPlanar(const Face &face)
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

void MergeCloseSorted(std::vector<double> &vals, double eps)
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
[[nodiscard]] bool ClipConvexPolygonVerticalLine(double uConst, const std::vector<glm::dvec2> &poly, double eps,
                                                  double &outSvLow, double &outSvHigh)
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
[[nodiscard]] bool ClipConvexPolygonHorizontalLine(double vConst, const std::vector<glm::dvec2> &poly, double eps,
                                                   double &outSuLow, double &outSuHigh)
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

/// Chord lies too close to the XY plane (unsupportable / “horizontal” in FDM with build-up +Z).
[[nodiscard]] bool ChordTooHorizontalForBuildUp(const glm::dvec3 &a0, const glm::dvec3 &a1,
                                                const glm::dvec3 &buildUpUnitZ, double minAbsDotZ)
{
    const glm::dvec3 chord = a1 - a0;
    const double len = glm::length(chord);
    if (!(len > 1.0e-9))
        return true;
    const glm::dvec3 dir = chord / len;
    return std::fabs(glm::dot(dir, buildUpUnitZ)) < minAbsDotZ;
}

void AppendRibRectangle(std::vector<std::pair<glm::vec3, glm::vec3>> &out, const glm::dvec3 &a0,
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

void BuildInteriorFaceRibs(const Scene &scene, const RibPreviewParams &params,
                           std::vector<std::pair<glm::vec3, glm::vec3>> &out)
{
    out.clear();
    constexpr int kMaxRibsPerAxis = 24;
    constexpr double kEps = 1.0e-9;
    constexpr double kUvEps = 1.0e-7;

    const double spacing = std::max(0.25, params.spacingMm);
    const double depthMm = std::max(0.0, params.depthMm);
    const double marginFrac = std::clamp(params.marginFrac, 0.01, 0.45);

    constexpr glm::dvec3 kBuildUpWorldZ(0.0, 0.0, 1.0);
    // Drop rib chords whose direction is nearly parallel to the print bed (|d·Z| tiny). Surviving
    // families run “more vertical” in world space; ~0.22 ≈ chords under ~12.7° elevation are removed.
    constexpr double kMinAbsChordDotBuildUpZ = 0.22;

    for (const Solid &solid : scene.solids)
    {
        for (const Face *fp : solid.faces)
        {
            if (fp == nullptr || fp->dependency != &solid)
                continue;
            const Face &face = *fp;

            if (face.loops.size() != 1)
                continue;
            if (!face.GetSurface().IsPlanar())
                continue;

            const glm::dvec3 nOut = OutwardNormalPlanar(face);

            // Near-horizontal lids/floors: in-plane ribbons are problematic for FDM without supports;
            // layer weakness along Z doesn’t motivate them strongly on caps. Assume build-up == world +Z until
            // a dedicated print-orientation axis exists (`RibPreviewParams` can grow that later).
            constexpr double kSkipRibsIfAbsCosNormalBuildUp =
                0.90; // |n·buildUp|; skip faces within ~25° of horizontal (top/bottom-ish)
            if (std::fabs(glm::dot(nOut, kBuildUpWorldZ)) > kSkipRibsIfAbsCosNormalBuildUp)
                continue;

            glm::dvec3 uAxis(1.0, 0.0, 0.0);
            if (std::abs(glm::dot(uAxis, nOut)) > 0.92)
                uAxis = glm::dvec3(0.0, 1.0, 0.0);

            glm::dvec3 u = glm::cross(nOut, uAxis);
            const double lu = glm::length(u);
            if (!(lu > kEps))
                continue;
            u /= lu;
            glm::dvec3 v = glm::cross(nOut, u);
            const double lv = glm::length(v);
            if (!(lv > kEps))
                continue;
            v /= lv;

            const glm::dvec3 inward = -nOut;

            std::vector<glm::dvec3> ring3d;
            ring3d.reserve(face.loops[0].size());
            for (const OrientedEdge &oe : face.loops[0])
            {
                if (oe.edge == nullptr)
                    continue;
                ring3d.push_back(oe.GetStartPosition());
            }
            if (ring3d.size() < 3)
                continue;

            glm::dvec3 centroid(0.0);
            for (const glm::dvec3 &p : ring3d)
                centroid += p;
            centroid /= static_cast<double>(ring3d.size());

            std::vector<glm::dvec2> ring2d;
            ring2d.reserve(ring3d.size());
            double umin = std::numeric_limits<double>::infinity();
            double umax = -std::numeric_limits<double>::infinity();
            double vmin = std::numeric_limits<double>::infinity();
            double vmax = -std::numeric_limits<double>::infinity();
            for (const glm::dvec3 &p : ring3d)
            {
                const glm::dvec3 r = p - centroid;
                const double su = glm::dot(r, u);
                const double sv = glm::dot(r, v);
                ring2d.emplace_back(su, sv);
                umin = std::min(umin, su);
                umax = std::max(umax, su);
                vmin = std::min(vmin, sv);
                vmax = std::max(vmax, sv);
            }

            const double spanU = umax - umin;
            const double spanV = vmax - vmin;
            if (!(spanU > kUvEps) || !(spanV > kUvEps))
                continue;

            const double marginU = marginFrac * spanU;
            const double marginV = marginFrac * spanV;

            auto emitConstantV = [&](double svLine)
            {
                double su0 = 0.0, su1 = 0.0;
                if (!ClipConvexPolygonHorizontalLine(svLine, ring2d, kUvEps, su0, su1))
                    return;
                const glm::dvec3 a0 = centroid + u * su0 + v * svLine;
                const glm::dvec3 a1 = centroid + u * su1 + v * svLine;
                if (ChordTooHorizontalForBuildUp(a0, a1, kBuildUpWorldZ, kMinAbsChordDotBuildUpZ))
                    return;
                AppendRibRectangle(out, a0, a1, inward, depthMm);
            };

            auto emitConstantU = [&](double suLine)
            {
                double sv0 = 0.0, sv1 = 0.0;
                if (!ClipConvexPolygonVerticalLine(suLine, ring2d, kUvEps, sv0, sv1))
                    return;
                const glm::dvec3 a0 = centroid + u * suLine + v * sv0;
                const glm::dvec3 a1 = centroid + u * suLine + v * sv1;
                if (ChordTooHorizontalForBuildUp(a0, a1, kBuildUpWorldZ, kMinAbsChordDotBuildUpZ))
                    return;
                AppendRibRectangle(out, a0, a1, inward, depthMm);
            };

            // Ribs parallel to +u (−− constant sv lines)
            {
                double v0 = vmin + marginV;
                const double v1 = vmax - marginV;
                int count = 0;
                for (; v0 <= v1 + spacing * 1.0e-6 && count < kMaxRibsPerAxis; v0 += spacing, ++count)
                    emitConstantV(v0);
            }
            // Ribs parallel to +v (constant su lines)
            {
                double u0 = umin + marginU;
                const double u1 = umax - marginU;
                int count = 0;
                for (; u0 <= u1 + spacing * 1.0e-6 && count < kMaxRibsPerAxis; u0 += spacing, ++count)
                    emitConstantU(u0);
            }
        }
    }
}

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
