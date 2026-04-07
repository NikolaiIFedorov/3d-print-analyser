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
    void Render();
    void Shutdown();

private:
    OpenGLShader shader;

    GLuint lineVAO = 0;
    GLuint lineVBO = 0;
    GLuint lineIBO = 0;
    uint32_t lineIndexCount = 0;

    glm::mat4 viewProjection = glm::mat4(1.0f);

    bool InitializeShaders();
    void Generate();
    void Upload(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);
};
