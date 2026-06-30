#pragma once

#include "glad/glad.h"
#include "SDL3/SDL.h"

#include "rendering/OpenGL/shaders/OpenGLShader.hpp"
#include "rendering/Camera/camera.hpp"
#include "rendering/color.hpp"
#include "scene/scene.hpp"
#include "logic/Analysis/AnalysisTypes.hpp"

#include <glm/glm.hpp>
#include <vector>

struct AnalysisVertex
{
    glm::vec3 position;
    glm::vec4 color;
};

class AnalysisRenderer
{
public:
    AnalysisRenderer() = default;
    explicit AnalysisRenderer(SDL_Window *window);
    ~AnalysisRenderer();

    AnalysisRenderer(const AnalysisRenderer &) = delete;
    AnalysisRenderer &operator=(const AnalysisRenderer &) = delete;
    AnalysisRenderer(AnalysisRenderer &&other) noexcept;
    AnalysisRenderer &operator=(AnalysisRenderer &&other) noexcept;

    void SetCamera(Camera &camera);

    /// Rebuild the overlay mesh from analysis results and upload to GPU.
    void Update(Scene *scene, const AnalysisResults &results);
    /// Clear GPU buffers without rebuilding.
    void Clear();

    /// Render the triangle overlay (alpha-blended, depth-correct).
    void RenderTriangles();
    /// Render the line overlay (currently unused, reserved for future use).
    void Render();

    void Shutdown();

private:
    bool InitializeShaders();

    void GenerateFaceOverlays(Scene *scene, const AnalysisResults &results,
                              std::vector<AnalysisVertex> &vertices,
                              std::vector<uint32_t> &indices) const;

    void UploadTriangles(const std::vector<AnalysisVertex> &vertices,
                         const std::vector<uint32_t> &indices);
    void UploadLines(const std::vector<AnalysisVertex> &vertices,
                     const std::vector<uint32_t> &indices);

    OpenGLShader shader;
    OpenGLShader lineShader;

    GLuint triangleVAO = 0;
    GLuint triangleVBO = 0;
    GLuint triangleIBO = 0;
    uint32_t triangleIndexCount = 0;

    GLuint lineVAO = 0;
    GLuint lineVBO = 0;
    GLuint lineIBO = 0;
    uint32_t lineIndexCount = 0;

    glm::mat4 viewProjection = glm::mat4(1.0f);
    float lineWidth = 2.0f;
};
