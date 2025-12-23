#include "camera.hpp"
#include "utils/log.hpp"

Camera::Camera(uint16_t width, uint16_t height)

{
    target = glm::vec3(0.0f, 0.0f, 0.0f);
    distance = 5.0f;
    azimuth = 0.0f;
    elevation = M_PI * 0.25f;
    orthoSize = 2.5f;
    aspectRatio = width / height;
    fov = 45.0f;
    nearPlane = 0;
    farPlane = INT_MAX;
}

glm::vec3 Camera::GetPosition() const
{
    float x = distance * std::cos(elevation) * std::sin(azimuth);
    float y = distance * std::sin(elevation);
    float z = distance * std::cos(elevation) * std::cos(azimuth);

    return target + glm::vec3(x, y, z);
}

glm::mat4 Camera::GetViewMatrix() const
{
    glm::vec3 position = GetPosition();
    glm::vec3 up(0.0f, 1.0f, 0.0f);

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
    float x = widthWindow / 2;
    float y = heightWindow / 2;
    target = {widthWindow, heightWindow, 0};

    float sensitivity = 0.1f;

    azimuth += deltaX * sensitivity;
    elevation -= deltaY * sensitivity;

    float maxElevation = M_PI * 0.5f - 0.01f;
    elevation = std::clamp(elevation, -maxElevation, maxElevation);
}

void Camera::Pan(float deltaX, float deltaY)
{
    glm::vec3 position = GetPosition();
    glm::vec3 forward = glm::normalize(target - position);
    glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
    glm::vec3 right = glm::normalize(glm::cross(forward, worldUp));
    glm::vec3 up = glm::normalize(glm::cross(right, forward));

    float sensitivity = distance * 0.01f;

    target -= right * (deltaX * sensitivity);
    target += up * (deltaY * sensitivity);
}

void Camera::Zoom(float delta, const glm::vec3 &targetPoint)
{
    float oldOrthoSize = orthoSize;

    float sensitivity = 0.1f;
    float zoomFactor = 1.0f - delta * sensitivity;

    orthoSize *= zoomFactor;
    orthoSize = std::clamp(orthoSize, 0.1f, 100.0f);

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
