#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>

class Camera
{
public:
    Camera() {};
    Camera(uint16_t width, uint16_t height);

    glm::mat4 GetViewMatrix() const;
    glm::mat4 GetProjectionMatrix() const;

    void Orbit(const float deltaX, const float deltaY);
    void Roll(const float delta);
    void Pan(const float deltaX, const float deltaY, bool scroll = true);
    void Zoom(const float delta, const glm::vec3 &targetPoint);

    void FrameBounds(const glm::vec3 &min, const glm::vec3 &max);

    void SetTarget(const glm::vec3 &target);
    void SetDistance(const float distance);
    void SetAspectRatio(float aspect, uint16_t width, uint16_t height);

    /// Target at origin, identity orientation (view toward XY from +Z), default distance and ortho zoom.
    void ResetHomeView();

    glm::vec3 *GetTarget() { return &target; }
    float GetDistance() const { return distance; }

    glm::vec3 target;
    float distance;
    glm::quat orientation;

    /// Principal-axis snap hysteresis latch state (angles are tuned via `UserTuning`).
    bool principalSnapLatched = false;
    glm::quat latchedPrincipalOrientation{1.0f, 0.0f, 0.0f, 0.0f};

    float orthoSize;
    float aspectRatio;
    float fov;
    float nearPlane;
    float farPlane;

    /// Logical window size for ortho scale / grid LOD (kept in sync with `SetAspectRatio`).
    uint16_t widthWindow = 1280;
    uint16_t heightWindow = 720;

    glm::vec3 GetPosition() const;

    /// True when the view direction is within the same cone as principal-axis snap (canonical top/front/side).
    bool IsPrincipalAxisView(float marginDegrees = 3.0f) const;
};