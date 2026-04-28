#include "ViewportRenderer.hpp"
#include "ProjectionDepthMode.hpp"
#include "RenderingExperiments.hpp"
#include "utils/log.hpp"

namespace
{
// Depth stacking (LEQUAL, smaller depth = nearer): scene uses default offset; grid is drawn first
// with a line offset so its stored depth wins over coplanar triangles/edges; axes use a stronger
// offset so they win over the grid. Signs flip with reverse-Z projection.
constexpr float kGridLinePolygonOffset = -0.9f;
constexpr float kAxisLinePolygonOffset = -2.25f;
} // namespace

ViewportRenderer::ViewportRenderer(SDL_Window *window)
{
    if (!InitializeShaders())
    {
        LOG_FALSE("Failed to initialize viewport shaders");
        return;
    }

    glGenVertexArrays(1, &lineVAO);

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
      principalSnapForGrid(other.principalSnapForGrid)
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
        principalSnapForGrid = other.principalSnapForGrid;
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

    principalSnapForGrid = camera.IsPrincipalAxisView() ? 1.0f : 0.0f;
}

void ViewportRenderer::SetAxisWorldHalfExtent(float halfLength)
{
    axisWorldHalfExtent = std::max(1.0f, halfLength);
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
    constexpr float spacing = 1.0f;

    glm::vec3 gridColor = Color::GetGrid();

    // Grid lines parallel to X axis (varying Y)
    for (float y = -extent; y <= extent + 1e-4f * spacing; y += spacing)
    {
        uint32_t base = static_cast<uint32_t>(vertices.size());
        vertices.push_back({glm::vec3(-extent, y, 0.0f), gridColor});
        vertices.push_back({glm::vec3(extent, y, 0.0f), gridColor});
        indices.push_back(base);
        indices.push_back(base + 1);
    }

    // Grid lines parallel to Y axis (varying X)
    for (float x = -extent; x <= extent + 1e-4f * spacing; x += spacing)
    {
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
    shader.SetVec3("uViewDirWorld", viewDirWorld);
    shader.SetFloat("uPrincipalSnap", principalSnapForGrid);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(RenderingExperiments::kReverseZDepth ? GL_GEQUAL : GL_LEQUAL);

    GLboolean blendWas = GL_FALSE;
    glGetBooleanv(GL_BLEND, &blendWas);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // Write depth so later passes can test against the floor plane.
    glDepthMask(GL_TRUE);

    // Bias grid slightly nearer than coplanar scene geometry so grid wins over mesh (see kGrid* / kAxis*).
    glEnable(GL_POLYGON_OFFSET_LINE);
    if (RenderingExperiments::kReverseZDepth)
        glPolygonOffset(-kGridLinePolygonOffset, -kGridLinePolygonOffset);
    else
        glPolygonOffset(kGridLinePolygonOffset, kGridLinePolygonOffset);

    glBindVertexArray(lineVAO);

    // Draw grid only — axes are handled by RenderAxes() after the scene
    glDrawElements(GL_LINES, gridIndexCount, GL_UNSIGNED_INT, 0);

    glBindVertexArray(0);

    glDisable(GL_POLYGON_OFFSET_LINE);

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
    shader.SetVec3("uViewDirWorld", viewDirWorld);
    shader.SetFloat("uPrincipalSnap", 0.0f);

    // Only draw where stencil == 0 (open space, not covered by solid geometry)
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_EQUAL, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(RenderingExperiments::kReverseZDepth ? GL_GEQUAL : GL_LEQUAL);
    glDepthMask(GL_FALSE);

    // Stronger line offset than grid so axes win depth over the floor grid where they coincide.
    glEnable(GL_POLYGON_OFFSET_LINE);
    if (RenderingExperiments::kReverseZDepth)
        glPolygonOffset(-kAxisLinePolygonOffset, -kAxisLinePolygonOffset);
    else
        glPolygonOffset(kAxisLinePolygonOffset, kAxisLinePolygonOffset);

    glBindVertexArray(lineVAO);
    glDrawElements(GL_LINES, axisIndexCount, GL_UNSIGNED_INT,
                   (void *)(gridIndexCount * sizeof(uint32_t)));
    glBindVertexArray(0);

    glDisable(GL_POLYGON_OFFSET_LINE);

    glDepthMask(GL_TRUE);
    glDisable(GL_STENCIL_TEST);
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
