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
    uint32_t triangleVertexCount = 0;
    size_t triangleVertexCapacity = 0;
    size_t triangleIndexCapacity = 0;

    GLuint lineVAO = 0;
    GLuint lineVBO = 0;
    GLuint lineIBO = 0;
    uint32_t lineIndexCount = 0;
    uint32_t lineVertexCount = 0;
    size_t lineVertexCapacity = 0;
    size_t lineIndexCapacity = 0;

    GLuint pickHighlightVAO = 0;
    GLuint pickHighlightVBO = 0;
    GLuint pickHighlightIBO = 0;
    uint32_t pickHighlightIndexCount = 0;

    GLuint pickHighlightLineVAO = 0;
    GLuint pickHighlightLineVBO = 0;
    GLuint pickHighlightLineIBO = 0;
    uint32_t pickHighlightLineIndexCount = 0;

    glm::mat4 viewMatrix = glm::mat4(1.0f);
    glm::mat4 projectionMatrix = glm::mat4(1.0f);
    glm::mat4 modelMatrix = glm::mat4(1.0f);
    glm::vec3 viewPos = glm::vec3(0.0f);
    glm::vec3 lightDir = glm::vec3(0.0f, 0.0f, 1.0f);

    float lineWidth = 3.0f;
    /// Multiplier on `LineShaderWireZNudgeNdc` for scene wireframe (principal views need a bit more nudge).
    float wireframeDepthNudgeScale = 1.0f;

    bool GetGLError(const std::source_location &loc = std::source_location::current());
    bool InitializeShaders();
    void DrawTrianglesPass(bool writeColor);

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
    bool UpdateTriangleMeshSubData(const std::vector<Vertex> &vertices, size_t vertexOffset,
                                   const std::vector<uint32_t> &indices, size_t indexOffset);
    void UploadPickHighlightMesh(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);
    void UploadPickHighlightLineMesh(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);
    void UploadLineMesh(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);
    bool UpdateLineMeshSubData(const std::vector<Vertex> &vertices, size_t vertexOffset,
                               const std::vector<uint32_t> &indices, size_t indexOffset);

    void DrawTriangles();
    void DrawPickHighlight();
    /// Screen-space thick lines (same pipeline as wireframe); `pixelWidth` is in framebuffer pixels.
    void DrawPickHighlightLines(float pixelWidth);
    void DrawLines();

    void SetWireFrameMode(bool enabled);
    void SetWireframeDepthNudgeScale(float scale) { wireframeDepthNudgeScale = scale; }
    void SetLineWidth(float width) { lineWidth = width; }
    float GetLineWidth() const { return lineWidth; }

    uint32_t GetTriangleIndexCount() const { return triangleIndexCount; }
    uint32_t GetLineIndexCount() const { return lineIndexCount; }
    uint32_t GetTriangleVertexCount() const { return triangleVertexCount; }
    uint32_t GetLineVertexCount() const { return lineVertexCount; }
};