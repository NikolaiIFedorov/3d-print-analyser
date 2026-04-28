#include "camera.hpp"
#include "utils/log.hpp"

#include <glm/gtc/matrix_inverse.hpp>

namespace
{
/// When target→camera is within this angle of world ±Z, snap to an exact plan pose (f = ±Z) with
/// roll taken from the current basis so the horizon does not jump arbitrarily. Avoids the
/// classic lookAt-style ill-conditioning without changing target, distance, or ortho zoom.
void SnapOrientationToCanonicalPlanIfNearWorldZ(glm::quat &orientation)
{
    constexpr float kPolarSnapRad = glm::radians(1.0f);

    const glm::mat3 M = glm::mat3_cast(orientation);
    glm::vec3 f = glm::normalize(M * glm::vec3(0.0f, 0.0f, 1.0f));
    if (!std::isfinite(f.x) || glm::length(f) < 1e-12f)
        return;

    const float polar = std::acos(std::clamp(f.z, -1.0f, 1.0f));
    if (polar > kPolarSnapRad && polar < glm::pi<float>() - kPolarSnapRad)
        return;

    const float zSign = (f.z >= 0.0f) ? 1.0f : -1.0f;
    const glm::vec3 fSnap(0.0f, 0.0f, zSign);

    glm::vec3 r0 = M * glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 rProj(r0.x, r0.y, 0.0f);
    const float hLen = glm::length(rProj);
    glm::vec3 r = (hLen > 1e-5f) ? (rProj * (1.0f / hLen)) : glm::vec3(1.0f, 0.0f, 0.0f);

    glm::vec3 u = glm::normalize(glm::cross(fSnap, r));
    if (!std::isfinite(u.x) || glm::length(u) < 1e-12f)
        u = glm::vec3(0.0f, zSign, 0.0f); // fSnap ∥ ±Z, r ⊥ Z ⇒ u along ±Y

    glm::quat q = glm::normalize(glm::quat_cast(glm::mat3(r, u, fSnap)));
    if (!std::isfinite(q.x))
        return;
    if (glm::dot(q, orientation) < 0.0f)
        q = -q;
    orientation = q;
}
} // namespace

Camera::Camera(uint16_t width, uint16_t height)

{
    target = glm::vec3(0.0f, 0.0f, 0.0f);
    distance = 5.0f;
    orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    orthoSize = 2.5f;
    aspectRatio = static_cast<float>(width) / static_cast<float>(height);
    fov = 45.0f;
    // Defaults until `Display::ApplyOrthoClipFromViewBounds` tightens the slab from scene +
    // grid + view-scaled axis extent (linear depth: precision ~ (far−near) / 2^24).
    nearPlane = -100000.0f;
    farPlane = 100000.0f;
}

glm::vec3 Camera::GetPosition() const
{
    glm::vec3 forward = orientation * glm::vec3(0.0f, 0.0f, 1.0f);
    return target + forward * distance;
}

glm::mat4 Camera::GetViewMatrix() const
{
    const glm::vec3 position = GetPosition();
    const glm::vec3 r = orientation * glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 u = orientation * glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 b = orientation * glm::vec3(0.0f, 0.0f, 1.0f); // target → camera

    // `glm::lookAt(eye, center, up)` becomes ill-conditioned when `up` is almost parallel to the
    // view ray; that shows up as a sudden scene flip near steep tilts. The turntable already
    // stores an orthonormal basis, so invert the camera-to-world rigid transform instead.
    const glm::mat4 camToWorld = glm::translate(glm::mat4(1.0f), position) * glm::mat4(glm::mat3(r, u, b));
    return glm::inverse(camToWorld);
}

glm::mat4 Camera::GetProjectionMatrix() const
{
    float halfWidth = orthoSize * aspectRatio;
    float halfHeight = orthoSize;

    return glm::ortho(
        -halfWidth, halfWidth,
        -halfHeight, halfHeight,
        nearPlane, farPlane);
}

void Camera::Orbit(float deltaX, float deltaY)
{
    // Turntable yaw about world +Z; pitch about camera **right** after that yaw (screen-horizontal
    // in world), not about cross(Z,f). The cross(Z,f) axis is the latitude tangent and matches
    // screen vertical only in special poses—oblique XZ→XY tilts often “stall” (~45°) because
    // mouse-y barely moves colatitude. Camera-right pitch tracks vertical drag intuitively.
    // R = R_pitch * R_yaw * R_current (explicit mat3); roll stays in R_current between frames.
    constexpr float kEps = 1e-6f;
    if (std::abs(deltaX) < kEps && std::abs(deltaY) < kEps)
        return;

    const glm::vec3 kWorldUp(0.0f, 0.0f, 1.0f);

    const glm::mat3 M_ori = glm::mat3_cast(orientation);
    const glm::vec3 f0 = glm::normalize(M_ori * glm::vec3(0.0f, 0.0f, 1.0f));
    if (!std::isfinite(f0.x) || glm::length(f0) < 1e-12f)
        return;

    glm::mat3 M_horizontal(1.0f);
    if (std::abs(deltaX) > kEps)
        M_horizontal = glm::mat3_cast(glm::angleAxis(-deltaX, kWorldUp));

    const glm::vec3 fAfterYaw = glm::normalize(M_horizontal * f0);

    glm::mat3 M_p(1.0f);
    if (std::abs(deltaY) > kEps)
    {
        glm::vec3 pitchAxis = glm::normalize(M_horizontal * M_ori * glm::vec3(1.0f, 0.0f, 0.0f));
        if (glm::length(pitchAxis) < 1e-6f)
        {
            pitchAxis = glm::cross(kWorldUp, fAfterYaw);
            const float paLen = glm::length(pitchAxis);
            if (paLen > 1e-6f)
                pitchAxis *= 1.0f / paLen;
            else
                pitchAxis = glm::vec3(1.0f, 0.0f, 0.0f);
        }

        M_p = glm::mat3_cast(glm::angleAxis(-deltaY, pitchAxis));
    }

    const glm::mat3 M_new = M_p * M_horizontal * M_ori;

    glm::quat qNew = glm::normalize(glm::quat_cast(M_new));
    if (!std::isfinite(qNew.x) || !std::isfinite(qNew.y) || !std::isfinite(qNew.z) || !std::isfinite(qNew.w))
        return;
    // quat_cast picks q or −q; choose the hemisphere continuous with the previous orientation.
    if (glm::dot(qNew, orientation) < 0.0f)
        qNew = -qNew;

    orientation = qNew;
    SnapOrientationToCanonicalPlanIfNearWorldZ(orientation);
}

void Camera::Roll(float delta)
{
    glm::vec3 forward = orientation * glm::vec3(0.0f, 0.0f, -1.0f);
    glm::quat rotation = glm::angleAxis(delta, forward);
    orientation = glm::normalize(rotation * orientation);
}

void Camera::Pan(float deltaX, float deltaY, bool scroll)
{
    glm::vec3 right = orientation * glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 up = orientation * glm::vec3(0.0f, 1.0f, 0.0f);

    float scaleX, scaleY;
    if (scroll)
    {
        scaleX = orthoSize * aspectRatio;
        scaleY = orthoSize;
    }
    else
    {
        scaleX = orthoSize;
        scaleY = orthoSize;
    }

    target -= right * (deltaX * scaleX);
    target += up * (deltaY * scaleY);
}

void Camera::Zoom(float delta, const glm::vec3 &targetPoint)
{
    float oldOrthoSize = orthoSize;

    float zoomFactor = 1.0f - delta;

    orthoSize *= zoomFactor;
    orthoSize = std::clamp(orthoSize, 0.001f, 10000.0f);

    float actualZoomFactor = orthoSize / oldOrthoSize;

    glm::vec3 toTarget = targetPoint - target;
    target += toTarget * (1.0f - actualZoomFactor);
}

void Camera::FrameBounds(const glm::vec3 &min, const glm::vec3 &max)
{
    target = (min + max) * 0.5f;

    glm::vec3 size = max - min;
    float maxDim = std::max({size.x, size.y, size.z});
    float halfDiag = glm::length(size) * 0.5f;

    orthoSize = maxDim * 0.6f;

    // Place camera just outside the bounding sphere so all geometry is in front.
    // (Distance has no effect on ortho zoom.)
    distance = halfDiag + 10.0f;

    // Keep near/far at their generous defaults so the world axes (±10000)
    // and grid are never clipped regardless of model size or zoom level.
}

void Camera::SetTarget(const glm::vec3 &t)
{
    target = t;
}

void Camera::SetDistance(float d)
{
    distance = d;
}

void Camera::SetAspectRatio(float aspect)
{
    aspectRatio = aspect;
}

void Camera::ResetHomeView()
{
    target = glm::vec3(0.0f, 0.0f, 0.0f);
    distance = 5.0f;
    orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    orthoSize = 2.5f;
}
