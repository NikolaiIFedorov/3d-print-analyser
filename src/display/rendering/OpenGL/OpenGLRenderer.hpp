#pragma once

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vector>
#include <iostream>

#include "shaders/Vertex.hpp"
#include "shaders/OpenGLShader.hpp"

#include "Goemetry/Geometry.hpp"

#include "source_location"

class OpenGLRenderer
{
private:
    GLFWwindow *window = nullptr;

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

    bool GetGLError(const std::source_location &loc = std::source_location::current());
    bool InitializeShaders();

public:
    OpenGLShader shader;

    OpenGLRenderer(GLFWwindow *window);
    ~OpenGLRenderer();

    bool Initialize(GLFWwindow *glfwWindow);
    void Shutdown();

    void BeginFrame();
    void EndFrame();
    void Clear(const glm::vec3 &color = glm::vec3(0.2f, 0.2f, 0.2f));

    void SetViewMatrix(const glm::mat4 &view);
    void SetProjectionMatrix(const glm::mat4 &projection);
    void SetModelMatrix(const glm::mat4 &model);

    void UploadTriangleMesh(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);
    void UploadLineMesh(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);

    void DrawTriangles();
    void DrawLines();

    void SetWireFrameMode(bool enabled);
};