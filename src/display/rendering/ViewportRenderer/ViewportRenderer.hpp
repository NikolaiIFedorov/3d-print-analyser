#pragma once

#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <vector>

#include "rendering/OpenGL/shaders/OpenGLShader.hpp"
#include "rendering/Camera/camera.hpp"
#include "rendering/color.hpp"
#include "Geometry/Geometry.hpp"

class ViewportRenderer
{
public:
    ViewportRenderer() = default;
    ViewportRenderer(SDL_Window *window);
    ~ViewportRenderer();

    ViewportRenderer(const ViewportRenderer &) = delete;
    ViewportRenderer &operator=(const ViewportRenderer &) = delete;
    ViewportRenderer(ViewportRenderer &&other) noexcept;
    ViewportRenderer &operator=(ViewportRenderer &&other) noexcept;

    void SetCamera(Camera &camera);
    /// World-space half-length of each axis ray from origin (must match ortho clip in Display).
    void SetAxisWorldHalfExtent(float halfLength);
    /// Minimum grid alpha when orbit is shallow (near-parallel to the XY plane).
    /// Ramps smoothly to fully opaque toward top-down-ish views (`Render()` tilt curve).
    void SetGridPlaneTiltMinOpacity(float minOpacity01);
    void Render();         // draws grid where the scene stencil is empty
    void RenderAxes();     // axes after scene — depth-tested, no stencil gate
    /// Same axis geometry as `RenderAxes`, but only where depth is **behind** stored scene Z (x-ray pass).
    /// Use with translucent shell so occluded axis segments read through the hull.
    void RenderAxesBehindScene();
    void RegenerateGrid(); // rebuild grid/axis vertex buffers (call after appearance change)
    void Shutdown();

private:
    OpenGLShader shader;

    GLuint lineVAO = 0;
    GLuint lineVBO = 0;
    GLuint lineIBO = 0;
    uint32_t lineIndexCount = 0;
    uint32_t gridIndexCount = 0;

    glm::mat4 viewProjection = glm::mat4(1.0f);
    /// Normalized world direction the camera looks (ortho: parallel view rays).
    glm::vec3 viewDirWorld{0.0f, 0.0f, -1.0f};
    float axisWorldHalfExtent = 10000.0f;
    /// World-space grid step; matches `Color::GRID_CELL_SIZE` (one default length unit in mm).
    float gridWorldSpacing = 1.0f;

    float gridPlaneTiltMinOpacity = 0.5f;
    /// Last `abs(farPlane - nearPlane)` from `SetCamera` (ortho depth slab span).
    float orthoClipDepthSpan = 200000.0f;
    /// Last `Camera::orthoSize` (ortho half-height in world units) for zoom-out bias scaling.
    float orthoHalfHeight = 2.5f;

    bool InitializeShaders();
    void Generate();
    void Upload(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);
    /// Clip-space Z bias for axes: nearer than grid, scaled when ortho `(far−near)` is wide.
    [[nodiscard]] float AxesClipZBiasW() const;
};
