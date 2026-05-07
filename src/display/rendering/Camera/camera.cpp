#include "camera.hpp"
#include "UserTuning.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <glm/gtc/matrix_inverse.hpp>
#include <optional>

namespace
{
struct PrincipalSnap
{
    glm::quat orientation;
    Camera::OrbitSnapAxis axis = Camera::OrbitSnapAxis::None;
};

Camera::OrbitSnapAxis SnapAxisForDirection(const glm::vec3 &fSnap)
{
    if (fSnap.x > 0.5f)
        return Camera::OrbitSnapAxis::PosX;
    if (fSnap.x < -0.5f)
        return Camera::OrbitSnapAxis::NegX;
    if (fSnap.y > 0.5f)
        return Camera::OrbitSnapAxis::PosY;
    if (fSnap.y < -0.5f)
        return Camera::OrbitSnapAxis::NegY;
    if (fSnap.z > 0.5f)
        return Camera::OrbitSnapAxis::PosZ;
    if (fSnap.z < -0.5f)
        return Camera::OrbitSnapAxis::NegZ;
    return Camera::OrbitSnapAxis::None;
}

glm::vec3 DirectionForSnapAxis(Camera::OrbitSnapAxis axis)
{
    switch (axis)
    {
    case Camera::OrbitSnapAxis::PosX:
        return glm::vec3(1.0f, 0.0f, 0.0f);
    case Camera::OrbitSnapAxis::NegX:
        return glm::vec3(-1.0f, 0.0f, 0.0f);
    case Camera::OrbitSnapAxis::PosY:
        return glm::vec3(0.0f, 1.0f, 0.0f);
    case Camera::OrbitSnapAxis::NegY:
        return glm::vec3(0.0f, -1.0f, 0.0f);
    case Camera::OrbitSnapAxis::PosZ:
        return glm::vec3(0.0f, 0.0f, 1.0f);
    case Camera::OrbitSnapAxis::NegZ:
        return glm::vec3(0.0f, 0.0f, -1.0f);
    case Camera::OrbitSnapAxis::None:
    default:
        return glm::vec3(0.0f);
    }
}

/// If world forward `orientation*(0,0,1)` lies within `acos(cosSnap)` of a world ±X/±Y/±Z axis,
/// returns the canonical orthographic snap quaternion; otherwise nullopt.
std::optional<PrincipalSnap> TryPrincipalSnapQuat(
    const glm::quat &orientation,
    float cosSnap,
    Camera::OrbitSnapAxis suppressedAxis = Camera::OrbitSnapAxis::None)
{
    const glm::mat3 M = glm::mat3_cast(orientation);
    glm::vec3 f = glm::normalize(M * glm::vec3(0.0f, 0.0f, 1.0f));
    if (!std::isfinite(f.x) || glm::length(f) < 1e-12f)
        return std::nullopt;

    const float ax = std::abs(f.x);
    const float ay = std::abs(f.y);
    const float az = std::abs(f.z);

    glm::vec3 fSnap(0.0f);
    if (ax >= cosSnap && ax >= ay && ax >= az)
        fSnap = glm::vec3(f.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f);
    else if (ay >= cosSnap && ay >= az)
        fSnap = glm::vec3(0.0f, f.y >= 0.0f ? 1.0f : -1.0f, 0.0f);
    else if (az >= cosSnap)
        fSnap = glm::vec3(0.0f, 0.0f, f.z >= 0.0f ? 1.0f : -1.0f);
    else
        return std::nullopt;

    const Camera::OrbitSnapAxis axis = SnapAxisForDirection(fSnap);
    if (axis == suppressedAxis)
        return std::nullopt;

    const glm::vec3 r0 = M * glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 rProj(0.0f);
    if (std::abs(fSnap.x) > 0.5f)
        rProj = glm::vec3(0.0f, r0.y, r0.z);
    else if (std::abs(fSnap.y) > 0.5f)
        rProj = glm::vec3(r0.x, 0.0f, r0.z);
    else
        rProj = glm::vec3(r0.x, r0.y, 0.0f);

    float hLen = glm::length(rProj);
    glm::vec3 r = (hLen > 1e-5f) ? (rProj * (1.0f / hLen)) : glm::vec3(0.0f);
    if (hLen <= 1e-5f)
    {
        if (std::abs(fSnap.x) > 0.5f)
            r = glm::vec3(0.0f, 1.0f, 0.0f);
        else if (std::abs(fSnap.y) > 0.5f)
            r = glm::vec3(0.0f, 0.0f, 1.0f);
        else
            r = glm::vec3(1.0f, 0.0f, 0.0f);
    }

    glm::vec3 u = glm::normalize(glm::cross(fSnap, r));
    if (!std::isfinite(u.x) || glm::length(u) < 1e-12f)
    {
        r = glm::normalize(glm::cross(
            fSnap, std::abs(fSnap.x) > 0.5f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f)));
        u = glm::normalize(glm::cross(fSnap, r));
    }

    glm::quat q = glm::normalize(glm::quat_cast(glm::mat3(r, u, fSnap)));
    if (!std::isfinite(q.x))
        return std::nullopt;
    if (glm::dot(q, orientation) < 0.0f)
        q = -q;
    return PrincipalSnap{q, axis};
}

std::optional<glm::quat> OrientationFromBackDirection(const glm::quat &currentOrientation, const glm::vec3 &backDirection)
{
    glm::vec3 b = glm::normalize(backDirection);
    if (!std::isfinite(b.x) || glm::length(b) < 1e-6f)
        return std::nullopt;

    glm::vec3 r = currentOrientation * glm::vec3(1.0f, 0.0f, 0.0f);
    r -= b * glm::dot(r, b);
    if (glm::length(r) < 1e-5f)
    {
        const glm::vec3 hint = std::abs(b.z) > 0.8f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
        r = glm::cross(hint, b);
    }
    if (glm::length(r) < 1e-5f)
        r = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), b);
    if (glm::length(r) < 1e-5f)
        return std::nullopt;

    r = glm::normalize(r);
    glm::vec3 u = glm::normalize(glm::cross(b, r));
    glm::quat q = glm::normalize(glm::quat_cast(glm::mat3(r, u, b)));
    if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w))
        return std::nullopt;
    if (glm::dot(q, currentOrientation) < 0.0f)
        q = -q;
    return q;
}

/// World-+Z turntable yaw moves the eye on a circle about `target`; `cross(Z, radialXY)` is the
/// tangent to that circle in the XY plane (positive ω). Compare to camera **right** so horizontal
/// mouse deltas keep the same screen-space swing when the view tilts (±1; +1 when ambiguous).
[[nodiscard]] float TurntableYawScreenAlignSign(const glm::mat3 &M_ori, const glm::vec3 &kWorldUp,
                                                float distance)
{
    const glm::vec3 toCamUnit = M_ori * glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 posRel = toCamUnit * distance;
    glm::vec3 radialXY(posRel.x, posRel.y, 0.0f);
    const float rl = glm::length(radialXY);
    constexpr float kRlMin = 1e-5f;
    if (rl <= kRlMin)
        return 1.0f;

    radialXY *= 1.0f / rl;
    glm::vec3 tangent = glm::normalize(glm::cross(kWorldUp, radialXY));
    glm::vec3 camR = glm::normalize(M_ori * glm::vec3(1.0f, 0.0f, 0.0f));
    if (!std::isfinite(camR.x) || glm::length(camR) < 1e-12f)
        return 1.0f;

    const float d = glm::dot(camR, tangent);
    constexpr float kDotDead = 1e-4f;
    if (std::abs(d) < kDotDead)
        return 1.0f;
    return glm::sign(d);
}
} // namespace

Camera::Camera(uint16_t width, uint16_t height)

{
    target = glm::vec3(0.0f, 0.0f, 0.0f);
    distance = 5.0f;
    orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    orthoSize = 2.5f;
    widthWindow = width;
    heightWindow = height;
    aspectRatio = static_cast<float>(width) / static_cast<float>(std::max<uint16_t>(1, height));
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

bool Camera::IsPrincipalAxisView(float marginDegrees) const
{
    return PrincipalSnapAxis(marginDegrees) != OrbitSnapAxis::None;
}

Camera::OrbitSnapAxis Camera::PrincipalSnapAxis(float marginDegrees) const
{
    const float cosSnap = std::cos(glm::radians(marginDegrees));
    if (auto snapped = TryPrincipalSnapQuat(orientation, cosSnap))
        return snapped->axis;
    return OrbitSnapAxis::None;
}

bool Camera::IsWithinSnapAxis(OrbitSnapAxis axis, float marginDegrees) const
{
    if (axis == OrbitSnapAxis::None)
        return false;
    const glm::vec3 axisDir = DirectionForSnapAxis(axis);
    const glm::mat3 M = glm::mat3_cast(orientation);
    const glm::vec3 f = glm::normalize(M * glm::vec3(0.0f, 0.0f, 1.0f));
    if (!std::isfinite(f.x) || glm::length(f) < 1e-12f)
        return false;
    return glm::dot(f, axisDir) >= std::cos(glm::radians(marginDegrees));
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
    // Turntable yaw about world +Z; pitch about camera **right** after that yaw (same as legacy).
    // Scale yaw by TurntableYawScreenAlignSign so horizontal drag keeps the same apparent left/right
    // swing on screen after tilts (camera right vs horizontal orbit tangent).
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
    {
        const float yawAlign = TurntableYawScreenAlignSign(M_ori, kWorldUp, distance);
        const float yawAngle = -deltaX * yawAlign;
        M_horizontal = glm::mat3_cast(glm::angleAxis(yawAngle, kWorldUp));
    }

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
}

Camera::OrbitSnapAxis Camera::SnapToPrincipalAxis(float snapDegrees, OrbitSnapAxis suppressedAxis)
{
    const float cosEnter = std::cos(glm::radians(std::max(0.0f, snapDegrees)));
    if (auto snapped = TryPrincipalSnapQuat(orientation, cosEnter, suppressedAxis))
    {
        orientation = snapped->orientation;
        return snapped->axis;
    }
    return OrbitSnapAxis::None;
}

Camera::OrbitSnapAxis Camera::FinishOrbitSnap(OrbitSnapAxis suppressedAxis)
{
    return SnapToPrincipalAxis(UserTuning::snapEnterDeg, suppressedAxis);
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

void Camera::FrameBoundsFromDirection(const glm::vec3 &min, const glm::vec3 &max, const glm::vec3 &cameraBackDirection)
{
    if (auto q = OrientationFromBackDirection(orientation, cameraBackDirection))
        orientation = *q;
    FrameBounds(min, max);
}

void Camera::SetTarget(const glm::vec3 &t)
{
    target = t;
}

void Camera::SetDistance(float d)
{
    distance = d;
}

void Camera::SetAspectRatio(float aspect, uint16_t width, uint16_t height)
{
    aspectRatio = aspect;
    widthWindow = width;
    heightWindow = height;
}

void Camera::ResetHomeView()
{
    target = glm::vec3(0.0f, 0.0f, 0.0f);
    distance = 5.0f;
    orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    orthoSize = 2.5f;
}
