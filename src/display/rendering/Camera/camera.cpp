#include "camera.hpp"
#include "utils/log.hpp"

Camera::Camera(uint16_t width, uint16_t height)

{
    target = glm::vec3(0.0f, 0.0f, 0.0f);
    distance = 5.0f;
    orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    orthoSize = 2.5f;
    aspectRatio = static_cast<float>(width) / static_cast<float>(height);
    fov = 45.0f;
    // Large symmetric range so nothing is near-clipped during orbit/pan.
    // Orthographic depth precision is linear, so ±100 000 world units still
    // gives ~12 µm per depth step with a 24-bit buffer — more than enough.
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
    glm::vec3 position = GetPosition();
    glm::vec3 up = orientation * glm::vec3(0.0f, 1.0f, 0.0f);

    return glm::lookAt(position, target, up);
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
    // Turntable + tilt (Plasticity-style):
    //  - Horizontal → yaw about fixed world +Z (part spins on the table).
    //  - Vertical → pitch about a horizontal axis (view tilts, Z can lean on screen).
    constexpr float kEps = 1e-6f;
    if (std::abs(deltaX) < kEps && std::abs(deltaY) < kEps)
        return;

    // Build / printer up — matches world axes used by the grid and modeling convention.
    const glm::vec3 kWorldUp(0.0f, 0.0f, 1.0f);

    if (std::abs(deltaX) > kEps)
    {
        glm::quat qYaw = glm::angleAxis(-deltaX, kWorldUp);
        orientation = glm::normalize(qYaw * orientation);
    }

    if (std::abs(deltaY) > kEps)
    {
        glm::vec3 forwardFromTarget = orientation * glm::vec3(0.0f, 0.0f, 1.0f);
        glm::vec3 viewIntoScene = -forwardFromTarget;
        // Pitch axis: horizontal in world, perpendicular to world Z and view. Vanishes
        // when the view is straight down or up; then use camera local +X in world
        // so vertical drag still tilts off the pole.
        glm::vec3 right = glm::cross(kWorldUp, viewIntoScene);
        float rlen = glm::length(right);
        if (rlen < 1e-4f)
        {
            right = orientation * glm::vec3(1.0f, 0.0f, 0.0f);
            rlen = glm::length(right);
        }
        if (rlen > 1e-4f)
        {
            right *= 1.0f / rlen;
            glm::quat qPitch = glm::angleAxis(-deltaY, right);
            orientation = glm::normalize(qPitch * orientation);
        }
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
