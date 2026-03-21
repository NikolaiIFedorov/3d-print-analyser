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
    nearPlane = -10000.0f;
    farPlane = 10000.0f;
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
        -100000.0f, 100000.0f);
}

void Camera::Orbit(float deltaX, float deltaY)
{
    float sensitivity = 0.01f;

    glm::vec3 right = orientation * glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 up = orientation * glm::vec3(0.0f, 1.0f, 0.0f);

    glm::quat yawRotation = glm::angleAxis(-deltaX * sensitivity, up);

    glm::quat pitchRotation = glm::angleAxis(-deltaY * sensitivity, right);

    orientation = glm::normalize(yawRotation * pitchRotation * orientation);
}

void Camera::Pan(float deltaX, float deltaY, bool scroll)
{
    glm::vec3 right = orientation * glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 up = orientation * glm::vec3(0.0f, 1.0f, 0.0f);

    float sensitivity = orthoSize * 0.01f;
    if (!scroll)
        sensitivity = orthoSize * 0.0025f;

    target -= right * (deltaX * sensitivity);
    target += up * (deltaY * sensitivity);
}

void Camera::Zoom(float delta, const glm::vec3 &targetPoint)
{
    float oldOrthoSize = orthoSize;

    float sensitivity = 0.1f;
    float zoomFactor = 1.0f - delta * sensitivity;

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

    orthoSize = maxDim * 0.6f;
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
