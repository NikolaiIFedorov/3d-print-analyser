#pragma once

#include "GLFW/glfw3.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

class Camera
{
public:
    Camera(uint16_t width, uint16_t height);

    glm::mat4 GetViewMatrix() const;
    glm::mat4 GetProjectionMatrix() const;

    void Orbit(const float deltaX, const float deltaY);
    void Pan(const float deltaX, const float deltaY);
    void Zoom(const float delta, const glm::vec3 &targetPoint);

    void FrameBounds(const glm::vec3 &min, const glm::vec3 &max);

    void SetTarget(const glm::vec3 &target);
    void SetDistance(const float distance);
    void SetAspectRatio(const float aspect);

    glm::vec3 *GetTarget() { return &target; }
    float GetDistance() const { return distance; }

    glm::vec3 target;
    float distance;
    float azimuth;
    float elevation;

    float orthoSize;
    float aspectRatio;
    float fov;
    float nearPlane;
    float farPlane;

    uint16_t widthWindow;
    uint16_t heightWindow;

private:
    glm::vec3 GetPosition() const;
};