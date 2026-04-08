#include "ViewportRenderer.hpp"
#include "utils/log.hpp"

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
      lineIBO(other.lineIBO), lineIndexCount(other.lineIndexCount),
      gridIndexCount(other.gridIndexCount),
      viewProjection(other.viewProjection)
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
    viewProjection = camera.GetProjectionMatrix() * camera.GetViewMatrix();
}

void ViewportRenderer::Generate()
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    const float extent = GRID_EXTENT;
    const float spacing = 1.0f;

    glm::vec3 gridColor = Color::GetGrid();

    // Grid lines parallel to X axis (varying Y)
    for (float y = -extent; y <= extent; y += spacing)
    {
        uint32_t base = static_cast<uint32_t>(vertices.size());
        vertices.push_back({glm::vec3(-extent, y, 0.0f), gridColor});
        vertices.push_back({glm::vec3(extent, y, 0.0f), gridColor});
        indices.push_back(base);
        indices.push_back(base + 1);
    }

    // Grid lines parallel to Y axis (varying X)
    for (float x = -extent; x <= extent; x += spacing)
    {
        uint32_t base = static_cast<uint32_t>(vertices.size());
        vertices.push_back({glm::vec3(x, -extent, 0.0f), gridColor});
        vertices.push_back({glm::vec3(x, extent, 0.0f), gridColor});
        indices.push_back(base);
        indices.push_back(base + 1);
    }

    gridIndexCount = static_cast<uint32_t>(indices.size());

    const float axisExtent = 10000.0f;

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

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);

    glBindVertexArray(lineVAO);

    // Draw grid
    glDrawElements(GL_LINES, gridIndexCount, GL_UNSIGNED_INT, 0);

    // Draw axes on top of grid
    glDisable(GL_DEPTH_TEST);
    uint32_t axisIndexCount = lineIndexCount - gridIndexCount;
    glDrawElements(GL_LINES, axisIndexCount, GL_UNSIGNED_INT,
                   (void *)(gridIndexCount * sizeof(uint32_t)));
    glEnable(GL_DEPTH_TEST);

    glBindVertexArray(0);
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
