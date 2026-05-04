#include "ViewportRenderer.hpp"
#include "ProjectionDepthMode.hpp"
#include "RenderingExperiments.hpp"
#include "rendering/color.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <cmath>

namespace
{
constexpr float kGridOpacityMin = 0.5f;
constexpr float kGridOpacityMax = 1.0f;
/// Below this |view·ẑ|, grid stays at min opacity (nearly parallel to XY plane).
constexpr float kGridPlaneTiltOpaqueStart = 0.08f;
/// From here to 1, ramp to full opacity (view more perpendicular to the reference plane).
constexpr float kGridPlaneTiltOpaqueEnd = 0.55f;

inline float Smoothstep01(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// Reference grid lies in XY; plane normal +ẑ. `absDotZ` = |viewDir·ẑ| in [0,1].
float GridOpacityFromPlaneTilt(float absDotZ)
{
    const float a = std::clamp(std::abs(absDotZ), 0.0f, 1.0f);
    float t = (a - kGridPlaneTiltOpaqueStart) /
              std::max(1.0e-6f, kGridPlaneTiltOpaqueEnd - kGridPlaneTiltOpaqueStart);
    t = Smoothstep01(t);
    return kGridOpacityMin + (kGridOpacityMax - kGridOpacityMin) * t;
}

} // namespace

ViewportRenderer::ViewportRenderer(SDL_Window *window)
{
    if (!InitializeShaders())
    {
        LOG_FALSE("Failed to initialize viewport shaders");
        return;
    }

    glGenVertexArrays(1, &lineVAO);

    gridWorldSpacing = std::max(1.0e-5f, Color::GRID_CELL_SIZE);
    Generate();

    LOG_VOID("Initialized ViewportRenderer");
}

ViewportRenderer::~ViewportRenderer()
{
    Shutdown();
}

ViewportRenderer::ViewportRenderer(ViewportRenderer &&other) noexcept
    : shader(std::move(other.shader)),
      lineVAO(other.lineVAO), lineVBO(other.lineVBO),
      lineIBO(other.lineIBO),       lineIndexCount(other.lineIndexCount),
      gridIndexCount(other.gridIndexCount),
      viewProjection(other.viewProjection),
      viewDirWorld(other.viewDirWorld),
      axisWorldHalfExtent(other.axisWorldHalfExtent),
      gridWorldSpacing(other.gridWorldSpacing),
      gridOpacityUserScale(other.gridOpacityUserScale)
{
    other.lineVAO = other.lineVBO = other.lineIBO = 0;
    other.lineIndexCount = 0;
    other.gridIndexCount = 0;
}

ViewportRenderer &ViewportRenderer::operator=(ViewportRenderer &&other) noexcept
{
    if (this != &other)
    {
        Shutdown();
        shader = std::move(other.shader);
        lineVAO = other.lineVAO;
        lineVBO = other.lineVBO;
        lineIBO = other.lineIBO;
        lineIndexCount = other.lineIndexCount;
        gridIndexCount = other.gridIndexCount;
        viewProjection = other.viewProjection;
        viewDirWorld = other.viewDirWorld;
        axisWorldHalfExtent = other.axisWorldHalfExtent;
        gridWorldSpacing = other.gridWorldSpacing;
        gridOpacityUserScale = other.gridOpacityUserScale;
        other.lineVAO = other.lineVBO = other.lineIBO = 0;
        other.lineIndexCount = 0;
        other.gridIndexCount = 0;
    }
    return *this;
}

bool ViewportRenderer::InitializeShaders()
{
    return shader.LoadFromFiles("shaders/basic.vert", "shaders/basic.frag");
}

void ViewportRenderer::SetCamera(Camera &camera)
{
    viewProjection = ProjectionDepthMode::EffectiveProjection(camera.GetProjectionMatrix()) *
                     camera.GetViewMatrix();
    const glm::vec3 forwardWorld = camera.orientation * glm::vec3(0.0f, 0.0f, 1.0f);
    const float fLen = glm::length(forwardWorld);
    if (fLen > 1e-8f)
        viewDirWorld = glm::normalize(-forwardWorld);

    const float fromColor = std::max(1.0e-5f, Color::GRID_CELL_SIZE);
    const float before = gridWorldSpacing;
    const float mag = std::max({1e-6f, before, fromColor});
    if (!std::isfinite(before) || std::abs(fromColor - before) > std::max(1e-7f, mag * 1e-5f))
    {
        gridWorldSpacing = fromColor;
        RegenerateGrid();
    }
}

void ViewportRenderer::SetAxisWorldHalfExtent(float halfLength)
{
    const float next = std::max(1.0f, halfLength);
    const float mag = std::max({1.0f, axisWorldHalfExtent, next});
    if (std::abs(next - axisWorldHalfExtent) <= std::max(1e-4f, mag * 1e-5f))
        return;

    axisWorldHalfExtent = next;
    RegenerateGrid();
}

void ViewportRenderer::SetGridOpacityUserScale(float scale)
{
    gridOpacityUserScale = std::clamp(scale, 0.0f, 1.0f);
}

void ViewportRenderer::RegenerateGrid()
{
    Generate();
}

void ViewportRenderer::Generate()
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    const float extent = Color::GRID_EXTENT;
    const float spacing = std::max(1.0f / 8192.0f, gridWorldSpacing);
    const int nSteps = std::max(1, static_cast<int>(std::floor(extent / spacing)));

    glm::vec3 gridColor = Color::GetGrid();

    // Grid lines parallel to X axis (varying Y)
    for (int iy = -nSteps; iy <= nSteps; ++iy)
    {
        const float y = static_cast<float>(iy) * spacing;
        if (std::abs(y) < spacing * 0.5f)
            continue;

        uint32_t base = static_cast<uint32_t>(vertices.size());
        vertices.push_back({glm::vec3(-extent, y, 0.0f), gridColor});
        vertices.push_back({glm::vec3(extent, y, 0.0f), gridColor});
        indices.push_back(base);
        indices.push_back(base + 1);
    }

    // Grid lines parallel to Y axis (varying X)
    for (int ix = -nSteps; ix <= nSteps; ++ix)
    {
        const float x = static_cast<float>(ix) * spacing;
        if (std::abs(x) < spacing * 0.5f)
            continue;

        uint32_t base = static_cast<uint32_t>(vertices.size());
        vertices.push_back({glm::vec3(x, -extent, 0.0f), gridColor});
        vertices.push_back({glm::vec3(x, extent, 0.0f), gridColor});
        indices.push_back(base);
        indices.push_back(base + 1);
    }

    gridIndexCount = static_cast<uint32_t>(indices.size());

    const float axisExtent = axisWorldHalfExtent;

    // X axis (negative then positive)
    uint32_t base = static_cast<uint32_t>(vertices.size());
    vertices.push_back({glm::vec3(-axisExtent, 0.0f, 0.0f), Color::GetAxisX(false)});
    vertices.push_back({glm::vec3(0.0f, 0.0f, 0.0f), Color::GetAxisX(false)});
    indices.push_back(base);
    indices.push_back(base + 1);

    base = static_cast<uint32_t>(vertices.size());
    vertices.push_back({glm::vec3(0.0f, 0.0f, 0.0f), Color::GetAxisX(true)});
    vertices.push_back({glm::vec3(axisExtent, 0.0f, 0.0f), Color::GetAxisX(true)});
    indices.push_back(base);
    indices.push_back(base + 1);

    // Y axis (negative then positive)
    base = static_cast<uint32_t>(vertices.size());
    vertices.push_back({glm::vec3(0.0f, -axisExtent, 0.0f), Color::GetAxisY(false)});
    vertices.push_back({glm::vec3(0.0f, 0.0f, 0.0f), Color::GetAxisY(false)});
    indices.push_back(base);
    indices.push_back(base + 1);

    base = static_cast<uint32_t>(vertices.size());
    vertices.push_back({glm::vec3(0.0f, 0.0f, 0.0f), Color::GetAxisY(true)});
    vertices.push_back({glm::vec3(0.0f, axisExtent, 0.0f), Color::GetAxisY(true)});
    indices.push_back(base);
    indices.push_back(base + 1);

    // Z axis (negative then positive)
    base = static_cast<uint32_t>(vertices.size());
    vertices.push_back({glm::vec3(0.0f, 0.0f, -axisExtent), Color::GetAxisZ(false)});
    vertices.push_back({glm::vec3(0.0f, 0.0f, 0.0f), Color::GetAxisZ(false)});
    indices.push_back(base);
    indices.push_back(base + 1);

    base = static_cast<uint32_t>(vertices.size());
    vertices.push_back({glm::vec3(0.0f, 0.0f, 0.0f), Color::GetAxisZ(true)});
    vertices.push_back({glm::vec3(0.0f, 0.0f, axisExtent), Color::GetAxisZ(true)});
    indices.push_back(base);
    indices.push_back(base + 1);

    Upload(vertices, indices);
}

void ViewportRenderer::Upload(const std::vector<Vertex> &vertices,
                              const std::vector<uint32_t> &indices)
{
    lineIndexCount = static_cast<uint32_t>(indices.size());
    if (lineIndexCount == 0)
        return;

    glBindVertexArray(lineVAO);

    if (lineVBO == 0)
        glGenBuffers(1, &lineVBO);
    glBindBuffer(GL_ARRAY_BUFFER, lineVBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex),
                 vertices.data(), GL_STATIC_DRAW);

    if (lineIBO == 0)
        glGenBuffers(1, &lineIBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lineIBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint32_t),
                 indices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void *)offsetof(Vertex, position));
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void *)offsetof(Vertex, color));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

void ViewportRenderer::Render()
{
    if (lineIndexCount == 0)
        return;

    shader.Use();
    shader.SetMat4("uViewProjection", viewProjection);
    shader.SetMat4("uModel", glm::mat4(1.0f));
    shader.SetFloat("uLightingEnabled", 0.0f);
    shader.SetFloat("uGridPlaneFade", 1.0f);
    shader.SetFloat("uGridOpacity",
                     std::clamp(GridOpacityFromPlaneTilt(viewDirWorld.z) * gridOpacityUserScale,
                                0.0f, 1.0f));
    shader.SetFloat("uClipZBiasW", RenderingExperiments::ClipZBiasGridW());
    shader.SetFloat("uAlpha", 1.0f);

    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_EQUAL, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(RenderingExperiments::kReverseZDepth ? GL_GEQUAL : GL_LEQUAL);

    GLboolean blendWas = GL_FALSE;
    glGetBooleanv(GL_BLEND, &blendWas);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // Write depth so later passes can test against the floor plane.
    glDepthMask(GL_TRUE);

    glBindVertexArray(lineVAO);

    // Draw grid only — axes are handled by RenderAxes() after the scene
    glDrawElements(GL_LINES, gridIndexCount, GL_UNSIGNED_INT, 0);

    glBindVertexArray(0);

    glDisable(GL_STENCIL_TEST);

    if (!blendWas)
        glDisable(GL_BLEND);
}

void ViewportRenderer::RenderAxes()
{
    if (lineIndexCount == 0)
        return;

    uint32_t axisIndexCount = lineIndexCount - gridIndexCount;
    if (axisIndexCount == 0)
        return;

    shader.Use();
    shader.SetMat4("uViewProjection", viewProjection);
    shader.SetMat4("uModel", glm::mat4(1.0f));
    shader.SetFloat("uLightingEnabled", 0.0f);
    shader.SetFloat("uGridPlaneFade", 0.0f);
    shader.SetFloat("uGridOpacity", 1.0f);
    shader.SetFloat("uClipZBiasW", RenderingExperiments::ClipZBiasAxesW());
    shader.SetFloat("uAlpha", 1.0f);

    // Depth already handles occlusion against scene geometry. Avoid stencil gating here:
    // near-origin zoom can leave stale/over-conservative stencil coverage that chops axis segments.
    glDisable(GL_STENCIL_TEST);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(RenderingExperiments::kReverseZDepth ? GL_GEQUAL : GL_LEQUAL);
    glDepthMask(GL_FALSE);

    glBindVertexArray(lineVAO);
    glDrawElements(GL_LINES, axisIndexCount, GL_UNSIGNED_INT,
                   (void *)(gridIndexCount * sizeof(uint32_t)));
    glBindVertexArray(0);

    glDepthMask(GL_TRUE);
}

void ViewportRenderer::Shutdown()
{
    shader.Delete();
    if (lineVBO)
        glDeleteBuffers(1, &lineVBO);
    if (lineIBO)
        glDeleteBuffers(1, &lineIBO);
    if (lineVAO)
        glDeleteVertexArrays(1, &lineVAO);

    lineVAO = lineVBO = lineIBO = 0;
    lineIndexCount = 0;
}
