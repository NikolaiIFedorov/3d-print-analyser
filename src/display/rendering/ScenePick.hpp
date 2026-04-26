#pragma once

#include <glm/glm.hpp>
#include <vector>

class Camera;
class Face;
struct Edge;

/// One tessellated triangle for CPU picking (matches Patch earcut output).
struct PickTriangle
{
    const Face *face = nullptr;
    glm::dvec3 v0{};
    glm::dvec3 v1{};
    glm::dvec3 v2{};
};

/// Straight or tessellated curve span for edge picking (shared `edge` across curve segments).
struct PickSegment
{
    const Edge *edge = nullptr;
    glm::dvec3 v0{};
    glm::dvec3 v1{};
};

/// Active tool / mode decides what is eligible for hover and pick.
enum class PickFilter : uint8_t
{
    None = 0,
    Faces = 1,
};

namespace ScenePick
{

/// NDC x,y in [-1,1] with y flipped for top-left window coordinates (same as ScreenToWorld).
void OrthoPickRay(const Camera &camera, int windowW, int windowH, float pixelX, float pixelY,
                  glm::dvec3 &outOrigin, glm::dvec3 &outDir);

/// Squared distance from a ray (origin, unit direction) to segment v0–v1; optionally returns the
/// closest point on the ray (for ray-parameter depth ordering).
double RayDistanceSqToSegment(glm::dvec3 rayOrigin, glm::dvec3 rayDir, glm::dvec3 v0, glm::dvec3 v1,
                              glm::dvec3 *optionalPointOnRay = nullptr);

/// Closest hit along +dir from origin; returns nullptr if none. `filter` reserved for future
/// primitive kinds (all entries are faces today). When `outRayT` is non-null, writes the ray
/// parameter (origin + t * dir) for the hit point.
const Face *PickClosestFace(const std::vector<PickTriangle> &triangles, glm::dvec3 rayOrigin,
                            glm::dvec3 rayDir, PickFilter filter, double *outRayT = nullptr);

/// Closest edge segment to the ray within `maxDistSq` (squared perpendicular distance in 3D).
/// When `outRayT` is non-null, writes the ray parameter to the closest point on the ray to the segment.
const Edge *PickClosestEdgeAlongRay(const std::vector<PickSegment> &segments, glm::dvec3 rayOrigin,
                                    glm::dvec3 rayDir, double maxDistSq, double *outRayT = nullptr,
                                    double *outDistSq = nullptr);

} // namespace ScenePick
