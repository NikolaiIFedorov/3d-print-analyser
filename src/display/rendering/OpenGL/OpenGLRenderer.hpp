#pragma once

#include <glad/glad.h>
#include <SDL3/SDL.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vector>
#include <iostream>

#include "shaders/Vertex.hpp"
#include "shaders/OpenGLShader.hpp"

#include "Geometry/Geometry.hpp"

#include "source_location"

class OpenGLRenderer
{
private:
    SDL_Window *window = nullptr;
    SDL_GLContext glContext = nullptr;

    GLuint triangleVAO = 0;
    GLuint triangleVBO = 0;
    GLuint triangleIBO = 0;
    uint32_t triangleIndexCount = 0;

    GLuint lineVAO = 0;
    GLuint lineVBO = 0;
    GLuint lineIBO = 0;
    uint32_t lineIndexCount = 0;

    glm::mat4 viewMatrix = glm::mat4(1.0f);
    glm::mat4 projectionMatrix = glm::mat4(1.0f);
    glm::mat4 modelMatrix = glm::mat4(1.0f);
    glm::vec3 viewPos = glm::vec3(0.0f);
    glm::vec3 lightDir = glm::vec3(0.0f, 0.0f, 1.0f);

    float lineWidth = 3.0f;

    bool GetGLError(const std::source_location &loc = std::source_location::current());
    bool InitializeShaders();

public:
    OpenGLShader shader;
    OpenGLShader lineShader;

    OpenGLRenderer() {};
    OpenGLRenderer(SDL_Window *window);
    ~OpenGLRenderer();

    OpenGLRenderer(const OpenGLRenderer &) = delete;
    OpenGLRenderer &operator=(const OpenGLRenderer &) = delete;
    OpenGLRenderer(OpenGLRenderer &&other) noexcept;
    OpenGLRenderer &operator=(OpenGLRenderer &&other) noexcept;

    void Shutdown();

    void EndFrame();
    void Clear(const glm::vec3 &color = glm::vec3(0.2f, 0.2f, 0.2f));

    void SetViewMatrix(const glm::mat4 &view);
    void SetProjectionMatrix(const glm::mat4 &projection);
    void SetModelMatrix(const glm::mat4 &model);
    void SetViewPos(const glm::vec3 &pos);

    void UploadTriangleMesh(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);
    void UploadLineMesh(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);

    void DrawTriangles();
    void DrawLines();

    void SetWireFrameMode(bool enabled);
    void SetLineWidth(float width) { lineWidth = width; }
    float GetLineWidth() const { return lineWidth; }
};