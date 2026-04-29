#include "ScenePick.hpp"

#include "ProjectionDepthMode.hpp"
#include "ViewportDepthExperiments.hpp"
#include "rendering/Camera/camera.hpp"

#include <cmath>
#include <limits>

namespace ScenePick
{

void OrthoPickRay(const Camera &camera, int windowW, int windowH, float pixelX, float pixelY,
                  glm::dvec3 &outOrigin, glm::dvec3 &outDir)
{
    if (windowW <= 0 || windowH <= 0)
    {
        outOrigin = glm::dvec3(0.0);
        outDir = glm::dvec3(0.0, 0.0, -1.0);
        return;
    }

    const float ndcX = 2.0f * pixelX / static_cast<float>(windowW) - 1.0f;
    const float ndcY = 1.0f - 2.0f * pixelY / static_cast<float>(windowH);

    const glm::mat4 proj =
        ProjectionDepthMode::EffectiveProjection(camera.GetProjectionMatrix());
    const glm::mat4 view = camera.GetViewMatrix();
    const glm::dmat4 invVP = glm::inverse(glm::dmat4(proj) * glm::dmat4(view));

    glm::dvec4 pNear = invVP * glm::dvec4(static_cast<double>(ndcX), static_cast<double>(ndcY), -1.0, 1.0);
    pNear /= pNear.w;
    glm::dvec4 pFar = invVP * glm::dvec4(static_cast<double>(ndcX), static_cast<double>(ndcY), 1.0, 1.0);
    pFar /= pFar.w;

    outOrigin = glm::dvec3(pNear);
    glm::dvec3 d = glm::dvec3(pFar) - outOrigin;
    const double len = glm::length(d);
    if (len < 1e-30)
        outDir = glm::dvec3(0.0, 0.0, -1.0);
    else
        outDir = d / len;
}

/// Möller–Trumbore; returns true with ray parameter t if hit (double-sided).
static bool RayTriangleMT(glm::dvec3 orig, glm::dvec3 dir, glm::dvec3 v0, glm::dvec3 v1, glm::dvec3 v2,
                          double &outT)
{
    constexpr double kEps = 1e-18;
    const glm::dvec3 e1 = v1 - v0;
    const glm::dvec3 e2 = v2 - v0;
    const glm::dvec3 p = glm::cross(dir, e2);
    const double det = glm::dot(e1, p);
    if (std::abs(det) < kEps)
        return false;
    const double invDet = 1.0 / det;
    const glm::dvec3 tvec = orig - v0;
    const double u = glm::dot(tvec, p) * invDet;
    if (u < 0.0 || u > 1.0)
        return false;
    const glm::dvec3 q = glm::cross(tvec, e1);
    const double v = glm::dot(dir, q) * invDet;
    if (v < 0.0 || u + v > 1.0)
        return false;
    const double t = glm::dot(e2, q) * invDet;
    if (t <= kEps)
        return false;
    outT = t;
    return true;
}

const Face *PickClosestFace(const std::vector<PickTriangle> &triangles, glm::dvec3 rayOrigin,
                            glm::dvec3 rayDir, PickFilter filter, double *outRayT)
{
    if (filter != PickFilter::Faces || triangles.empty())
    {
        if (outRayT != nullptr)
            *outRayT = 0.0;
        return nullptr;
    }

    const double dirLen = glm::length(rayDir);
    if (dirLen < 1e-30)
    {
        if (outRayT != nullptr)
            *outRayT = 0.0;
        return nullptr;
    }
    rayDir /= dirLen;

    double bestT = std::numeric_limits<double>::infinity();
    const Face *bestFace = nullptr;

    for (const PickTriangle &tri : triangles)
    {
        if (tri.face == nullptr)
            continue;
        double t = 0.0;
        if (!RayTriangleMT(rayOrigin, rayDir, tri.v0, tri.v1, tri.v2, t))
            continue;

        if (ViewportDepthExperiments::IsPickFrontFaceOnly())
        {
            const glm::dvec3 e1 = tri.v1 - tri.v0;
            const glm::dvec3 e2 = tri.v2 - tri.v0;
            glm::dvec3 n = glm::cross(e1, e2);
            const double nLen = glm::length(n);
            if (nLen < 1e-30)
                continue;
            n /= nLen;
            if (glm::dot(n, rayDir) >= 0.0)
                continue;
        }

        if (t < bestT)
        {
            bestT = t;
            bestFace = tri.face;
        }
    }
    if (outRayT != nullptr)
        *outRayT = bestFace != nullptr ? bestT : 0.0;
    return bestFace;
}

double RayDistanceSqToSegment(glm::dvec3 rayOrigin, glm::dvec3 rayDir, glm::dvec3 v0, glm::dvec3 v1,
                            glm::dvec3 *optionalPointOnRay)
{
    const double segLen = glm::distance(v0, v1);
    if (segLen < 1e-20)
    {
        const glm::dvec3 w = v0 - rayOrigin;
        const double s0 = std::max(0.0, glm::dot(w, rayDir));
        const glm::dvec3 pr = rayOrigin + rayDir * s0;
        if (optionalPointOnRay != nullptr)
            *optionalPointOnRay = pr;
        return glm::dot(pr - v0, pr - v0);
    }

    // Port of three.js Ray.distanceSqToSegment (GTE DistRaySegment).
    const glm::dvec3 segCenter = (v0 + v1) * 0.5;
    glm::dvec3 segDir = v1 - v0;
    segDir /= segLen;
    const glm::dvec3 diff = rayOrigin - segCenter;
    const double segExtent = segLen * 0.5;
    const double a01 = -glm::dot(rayDir, segDir);
    const double b0 = glm::dot(rayDir, diff);
    const double b1 = -glm::dot(segDir, diff);
    const double c = glm::dot(diff, diff);
    const double det = std::abs(1.0 - a01 * a01);
    double s0 = 0.0;
    double s1 = 0.0;
    double sqrDist = 0.0;

    if (det > 1e-20)
    {
        const double extDet = segExtent * det;
        double s0n = a01 * b1 - b0;
        double s1n = a01 * b0 - b1;

        if (s0n >= 0.0)
        {
            if (s1n >= -extDet)
            {
                if (s1n <= extDet)
                {
                    const double invDet = 1.0 / det;
                    s0 = s0n * invDet;
                    s1 = s1n * invDet;
                    sqrDist = s0 * (s0 + a01 * s1 + 2.0 * b0) + s1 * (a01 * s0 + s1 + 2.0 * b1) + c;
                }
                else
                {
                    s1 = segExtent;
                    s0 = std::max(0.0, -(a01 * s1 + b0));
                    sqrDist = -s0 * s0 + s1 * (s1 + 2.0 * b1) + c;
                }
            }
            else
            {
                s1 = -segExtent;
                s0 = std::max(0.0, -(a01 * s1 + b0));
                sqrDist = -s0 * s0 + s1 * (s1 + 2.0 * b1) + c;
            }
        }
        else
        {
            if (s1n <= -extDet)
            {
                s0 = std::max(0.0, -(-a01 * segExtent + b0));
                s1 = (s0 > 0.0) ? -segExtent : std::min(std::max(-segExtent, -b1), segExtent);
                sqrDist = -s0 * s0 + s1 * (s1 + 2.0 * b1) + c;
            }
            else if (s1n <= extDet)
            {
                s0 = 0.0;
                s1 = std::min(std::max(-segExtent, -b1), segExtent);
                sqrDist = s1 * (s1 + 2.0 * b1) + c;
            }
            else
            {
                s0 = std::max(0.0, -(a01 * segExtent + b0));
                s1 = (s0 > 0.0) ? segExtent : std::min(std::max(-segExtent, -b1), segExtent);
                sqrDist = -s0 * s0 + s1 * (s1 + 2.0 * b1) + c;
            }
        }
    }
    else
    {
        s1 = (a01 > 0.0) ? -segExtent : segExtent;
        s0 = std::max(0.0, -(a01 * s1 + b0));
        sqrDist = -s0 * s0 + s1 * (s1 + 2.0 * b1) + c;
    }

    if (optionalPointOnRay != nullptr)
        *optionalPointOnRay = rayOrigin + rayDir * s0;
    return sqrDist;
}

const Edge *PickClosestEdgeAlongRay(const std::vector<PickSegment> &segments, glm::dvec3 rayOrigin,
                                     glm::dvec3 rayDir, double maxDistSq, double *outRayT,
                                     double *outDistSq)
{
    if (segments.empty() || maxDistSq < 0.0)
    {
        if (outRayT != nullptr)
            *outRayT = 0.0;
        if (outDistSq != nullptr)
            *outDistSq = std::numeric_limits<double>::infinity();
        return nullptr;
    }

    const double dirLen = glm::length(rayDir);
    if (dirLen < 1e-30)
    {
        if (outRayT != nullptr)
            *outRayT = 0.0;
        if (outDistSq != nullptr)
            *outDistSq = std::numeric_limits<double>::infinity();
        return nullptr;
    }
    rayDir /= dirLen;

    const Edge *bestEdge = nullptr;
    double bestDistSq = std::numeric_limits<double>::infinity();
    double bestRayT = 0.0;

    for (const PickSegment &ps : segments)
    {
        if (ps.edge == nullptr)
            continue;
        glm::dvec3 ptOnRay{};
        const double d2 = RayDistanceSqToSegment(rayOrigin, rayDir, ps.v0, ps.v1, &ptOnRay);
        if (d2 > maxDistSq)
            continue;
        const double t = glm::dot(ptOnRay - rayOrigin, rayDir);
        if (t < 0.0)
            continue;
        if (d2 < bestDistSq || (std::abs(d2 - bestDistSq) <= 1e-24 && t < bestRayT))
        {
            bestDistSq = d2;
            bestRayT = t;
            bestEdge = ps.edge;
        }
    }

    if (outRayT != nullptr)
        *outRayT = bestEdge != nullptr ? bestRayT : 0.0;
    if (outDistSq != nullptr)
        *outDistSq = bestEdge != nullptr ? bestDistSq : std::numeric_limits<double>::infinity();
    return bestEdge;
}

} // namespace ScenePick
