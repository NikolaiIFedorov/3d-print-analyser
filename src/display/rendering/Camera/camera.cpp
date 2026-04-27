#include "camera.hpp"
#include "utils/log.hpp"

#include <glm/gtc/matrix_inverse.hpp>

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
    // Turntable: default yaw about world +Z (azimuthZ). Near ±Z, Rz leaves f unchanged, so use
    // world +X for horizontal drag (azimuthX / X-chart) so orbit still moves f on the sphere.
    // Build R = R_pitch * R_horizontal * R_current; roll stays in R_current between frames.
    constexpr float kEps = 1e-6f;
    if (std::abs(deltaX) < kEps && std::abs(deltaY) < kEps)
        return;

    const glm::vec3 kWorldUp(0.0f, 0.0f, 1.0f);
    const glm::vec3 kWorldX(1.0f, 0.0f, 0.0f);

    const glm::mat3 M_ori = glm::mat3_cast(orientation);
    const glm::vec3 f0 = glm::normalize(M_ori * glm::vec3(0.0f, 0.0f, 1.0f));
    if (!std::isfinite(f0.x) || glm::length(f0) < 1e-12f)
        return;

    // Within ~10° of straight over/under the target, |f·Z| > cos(80°) ≈ 0.1736.
    const float kPoleChartCos = glm::cos(glm::radians(80.0f));

    glm::mat3 M_horizontal(1.0f);
    if (std::abs(deltaX) > kEps)
    {
        if (f0.z > kPoleChartCos)
        {
            // North cap: horizontal drag → Rx so f moves in the YZ plane (Rz would fix +Z).
            M_horizontal = glm::mat3_cast(glm::angleAxis(-deltaX, kWorldX));
        }
        else if (f0.z < -kPoleChartCos)
        {
            // South cap: opposite handedness so the same mouse motion curls f off −Z similarly.
            M_horizontal = glm::mat3_cast(glm::angleAxis(deltaX, kWorldX));
        }
        else
        {
            M_horizontal = glm::mat3_cast(glm::angleAxis(-deltaX, kWorldUp));
        }
    }

    const glm::vec3 fAfterYaw = glm::normalize(M_horizontal * f0);

    glm::mat3 M_p(1.0f);
    if (std::abs(deltaY) > kEps)
    {
        glm::vec3 pitchAxis = glm::cross(kWorldUp, fAfterYaw);
        const float paLen = glm::length(pitchAxis);
        if (paLen > 1e-6f)
            pitchAxis *= 1.0f / paLen;
        else
            pitchAxis = glm::vec3(1.0f, 0.0f, 0.0f); // over the pole: pitch in XZ

        M_p = glm::mat3_cast(glm::angleAxis(-deltaY, pitchAxis));
    }

    const glm::mat3 M_new = M_p * M_horizontal * M_ori;

    glm::quat qNew = glm::normalize(glm::quat_cast(M_new));
    if (!std::isfinite(qNew.x) || !std::isfinite(qNew.y) || !std::isfinite(qNew.z) || !std::isfinite(qNew.w) ||
        glm::dot(qNew, qNew) < 0.25f)
        return;

    // Only snap near true ±Z for view-matrix stability (GetViewMatrix); margin is tiny so a real
    // plan view of the XY plane (camera over +Z or under −Z) is still reachable.
    constexpr float kPolarMargin = 1e-5f;

    glm::vec3 f = glm::normalize(M_new * glm::vec3(0.0f, 0.0f, 1.0f));
    if (!std::isfinite(f.x) || glm::length(f) < 1e-12f)
        return;

    float polar = std::acos(std::clamp(f.z, -1.0f, 1.0f));
    if (polar < kPolarMargin || polar > glm::pi<float>() - kPolarMargin)
    {
        glm::vec2 hz(f.x, f.y);
        float hLen = glm::length(hz);
        if (hLen < 1e-4f)
        {
            hz = glm::vec2(fAfterYaw.x, fAfterYaw.y);
            hLen = glm::length(hz);
        }
        if (hLen < 1e-4f)
            hz = glm::vec2(1.0f, 0.0f);
        else
            hz /= hLen;
        polar = glm::clamp(polar, kPolarMargin, glm::pi<float>() - kPolarMargin);
        const float sinP = std::sin(polar);
        f = glm::normalize(glm::vec3(hz.x * sinP, hz.y * sinP, std::cos(polar)));

        glm::vec3 r = glm::cross(kWorldUp, f);
        const float rLen = glm::length(r);
        if (rLen > 1e-6f)
            r *= 1.0f / rLen;
        else
            r = glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 u = glm::normalize(glm::cross(f, r));
        orientation = glm::normalize(glm::quat_cast(glm::mat3(r, u, f)));
    }
    else
    {
        orientation = qNew;
    }
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
