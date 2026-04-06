#include "AnalysisRenderer.hpp"
#include <mapbox/earcut.hpp>
#include "utils/log.hpp"

AnalysisRenderer::AnalysisRenderer(SDL_Window *window)
{
    if (!InitializeShaders())
    {
        LOG_FALSE("Failed to initialize analysis shaders");
        return;
    }

    glGenVertexArrays(1, &triangleVAO);
    glGenVertexArrays(1, &lineVAO);

    LOG_VOID("Initialized AnalysisRenderer");
}

AnalysisRenderer::~AnalysisRenderer()
{
    Shutdown();
}

AnalysisRenderer::AnalysisRenderer(AnalysisRenderer &&other) noexcept
    : shader(std::move(other.shader)),
      triangleVAO(other.triangleVAO), triangleVBO(other.triangleVBO),
      triangleIBO(other.triangleIBO), triangleIndexCount(other.triangleIndexCount),
      lineVAO(other.lineVAO), lineVBO(other.lineVBO),
      lineIBO(other.lineIBO), lineIndexCount(other.lineIndexCount),
      viewProjection(other.viewProjection)
{
    other.triangleVAO = other.triangleVBO = other.triangleIBO = 0;
    other.lineVAO = other.lineVBO = other.lineIBO = 0;
    other.triangleIndexCount = other.lineIndexCount = 0;
}

AnalysisRenderer &AnalysisRenderer::operator=(AnalysisRenderer &&other) noexcept
{
    if (this != &other)
    {
        Shutdown();
        shader = std::move(other.shader);
        triangleVAO = other.triangleVAO;
        triangleVBO = other.triangleVBO;
        triangleIBO = other.triangleIBO;
        triangleIndexCount = other.triangleIndexCount;
        lineVAO = other.lineVAO;
        lineVBO = other.lineVBO;
        lineIBO = other.lineIBO;
        lineIndexCount = other.lineIndexCount;
        viewProjection = other.viewProjection;
        other.triangleVAO = other.triangleVBO = other.triangleIBO = 0;
        other.lineVAO = other.lineVBO = other.lineIBO = 0;
        other.triangleIndexCount = other.lineIndexCount = 0;
    }
    return *this;
}

bool AnalysisRenderer::InitializeShaders()
{
    return shader.LoadFromFiles("shaders/analysis.vert", "shaders/analysis.frag");
}

void AnalysisRenderer::SetCamera(Camera &camera)
{
    viewProjection = camera.GetProjectionMatrix() * camera.GetViewMatrix();
}

void AnalysisRenderer::Update(Scene *scene, const AnalysisResults &results)
{
    std::vector<AnalysisVertex> triVerts;
    std::vector<uint32_t> triIndices;
    GenerateFaceOverlays(scene, results, triVerts, triIndices);
    UploadTriangles(triVerts, triIndices);

    std::vector<AnalysisVertex> lineVerts;
    std::vector<uint32_t> lineIndices;
    GenerateLayerLines(results, lineVerts, lineIndices);
    UploadLines(lineVerts, lineIndices);
}

void AnalysisRenderer::Clear()
{
    triangleIndexCount = 0;
    lineIndexCount = 0;
}

// -- Face overlay generation (re-triangulates flagged faces with overlay color) --

static void CreatePlaneCoordinateSystem(const glm::dvec3 &normal,
                                        glm::dvec3 &uAxis, glm::dvec3 &vAxis)
{
    glm::dvec3 arbitrary = (std::abs(normal.x) < 0.9)
                               ? glm::dvec3(1, 0, 0)
                               : glm::dvec3(0, 1, 0);
    uAxis = glm::normalize(glm::cross(normal, arbitrary));
    vAxis = glm::normalize(glm::cross(normal, uAxis));
}

static glm::dvec2 ProjectPointToPlane(const glm::dvec3 &point3D,
                                      const glm::dvec3 &origin,
                                      const glm::dvec3 &uAxis,
                                      const glm::dvec3 &vAxis)
{
    glm::dvec3 rel = point3D - origin;
    return glm::dvec2(glm::dot(rel, uAxis), glm::dot(rel, vAxis));
}

static std::vector<glm::dvec3> TessellateCurve(const Curve *curve,
                                               const glm::dvec3 &start,
                                               const glm::dvec3 &end,
                                               int segments)
{
    std::vector<glm::dvec3> points;
    if (curve == nullptr)
    {
        points.push_back(start);
        points.push_back(end);
        return points;
    }
    for (int i = 0; i <= segments + 1; i++)
    {
        double t = (double)i / segments;
        points.push_back(curve->Evaluate(t, start, end));
    }
    return points;
}

static void TriangulateFace(const Face *face, const glm::vec4 &color,
                            std::vector<AnalysisVertex> &vertices,
                            std::vector<uint32_t> &indices)
{
    glm::dvec3 faceNormal = face->GetSurface().GetNormal();
    glm::dvec3 uAxis, vAxis;
    CreatePlaneCoordinateSystem(faceNormal, uAxis, vAxis);

    using Point2D = std::array<double, 2>;
    std::vector<std::vector<Point2D>> polygon;
    std::vector<std::vector<glm::dvec3>> allLoopPositions;

    glm::dvec3 projectionOrigin(0, 0, 0);
    bool originSet = false;

    for (const auto &edgeLoop : face->loops)
    {
        std::vector<glm::dvec3> loopPositions;

        for (const auto &orientedEdge : edgeLoop)
        {
            const Point *p0 = orientedEdge.GetStart();
            if (!originSet)
            {
                projectionOrigin = p0->position;
                originSet = true;
            }

            if (orientedEdge.edge->curve == nullptr)
            {
                loopPositions.push_back(p0->position);
            }
            else
            {
                auto tessellated = TessellateCurve(
                    orientedEdge.edge->curve,
                    orientedEdge.GetStartPosition(),
                    orientedEdge.GetEndPosition(), 16);
                for (size_t j = 0; j < tessellated.size() - 1; j++)
                    loopPositions.push_back(tessellated[j]);
            }
        }

        allLoopPositions.push_back(loopPositions);

        std::vector<Point2D> loop2D;
        for (const glm::dvec3 &pos : loopPositions)
        {
            glm::dvec2 pos2D = ProjectPointToPlane(pos, projectionOrigin, uAxis, vAxis);
            loop2D.push_back({pos2D.x, pos2D.y});
        }
        polygon.push_back(loop2D);
    }

    if (polygon.empty())
        return;

    auto triangleIndices = mapbox::earcut<uint32_t>(polygon);
    if (triangleIndices.empty())
        return;

    uint32_t baseIndex = vertices.size();
    for (const auto &loopPositions : allLoopPositions)
    {
        for (const glm::dvec3 &pos : loopPositions)
            vertices.push_back({glm::vec3(pos), color});
    }

    for (uint32_t idx : triangleIndices)
        indices.push_back(baseIndex + idx);
}

void AnalysisRenderer::GenerateFaceOverlays(Scene *scene, const AnalysisResults &results,
                                            std::vector<AnalysisVertex> &vertices,
                                            std::vector<uint32_t> &indices) const
{
    for (const auto &[face, flaw] : results.faceFlaws)
    {
        if (flaw == Flaw::NONE)
            continue;
        TriangulateFace(face, GetFaceOverlayColor(flaw), vertices, indices);
    }
}

void AnalysisRenderer::GenerateLayerLines(const AnalysisResults &results,
                                          std::vector<AnalysisVertex> &vertices,
                                          std::vector<uint32_t> &indices) const
{
    for (const auto &[solid, layers] : results.solidLayers)
    {
        for (const auto &layer : layers)
        {
            glm::vec4 color = GetLayerColor(layer.flaw);
            for (const auto &seg : layer.segments)
            {
                uint32_t base = vertices.size();
                vertices.push_back({glm::vec3(seg.a), color});
                vertices.push_back({glm::vec3(seg.b), color});
                indices.push_back(base);
                indices.push_back(base + 1);
            }
        }
    }
}

// -- Colors --

glm::vec4 AnalysisRenderer::GetFaceOverlayColor(Flaw flaw)
{
    switch (flaw)
    {
    case Flaw::OVERHANG:
        return glm::vec4(0.8f, 0.2f, 0.2f, 0.35f);
    default:
        return glm::vec4(0.0f);
    }
}

glm::vec4 AnalysisRenderer::GetLayerColor(Flaw flaw)
{
    switch (flaw)
    {
    case Flaw::THIN_SECTION:
        return glm::vec4(0.9f, 0.3f, 0.3f, 1.0f);
    case Flaw::SHARP_CORNER:
        return glm::vec4(0.3f, 0.9f, 0.3f, 1.0f);
    case Flaw::BRIDGING:
        return glm::vec4(0.3f, 0.3f, 0.9f, 1.0f);
    case Flaw::SMALL_FEATURE:
        return glm::vec4(0.9f, 0.9f, 0.3f, 1.0f);
    default:
        return glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
    }
}

// -- GL resource management --

void AnalysisRenderer::UploadTriangles(const std::vector<AnalysisVertex> &vertices,
                                       const std::vector<uint32_t> &indices)
{
    triangleIndexCount = indices.size();
    if (triangleIndexCount == 0)
        return;

    glBindVertexArray(triangleVAO);

    if (triangleVBO == 0)
        glGenBuffers(1, &triangleVBO);
    glBindBuffer(GL_ARRAY_BUFFER, triangleVBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(AnalysisVertex),
                 vertices.data(), GL_DYNAMIC_DRAW);

    if (triangleIBO == 0)
        glGenBuffers(1, &triangleIBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, triangleIBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint32_t),
                 indices.data(), GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(AnalysisVertex),
                          (void *)offsetof(AnalysisVertex, position));
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(AnalysisVertex),
                          (void *)offsetof(AnalysisVertex, color));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

void AnalysisRenderer::UploadLines(const std::vector<AnalysisVertex> &vertices,
                                   const std::vector<uint32_t> &indices)
{
    lineIndexCount = indices.size();
    if (lineIndexCount == 0)
        return;

    glBindVertexArray(lineVAO);

    if (lineVBO == 0)
        glGenBuffers(1, &lineVBO);
    glBindBuffer(GL_ARRAY_BUFFER, lineVBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(AnalysisVertex),
                 vertices.data(), GL_DYNAMIC_DRAW);

    if (lineIBO == 0)
        glGenBuffers(1, &lineIBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lineIBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint32_t),
                 indices.data(), GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(AnalysisVertex),
                          (void *)offsetof(AnalysisVertex, position));
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(AnalysisVertex),
                          (void *)offsetof(AnalysisVertex, color));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

void AnalysisRenderer::Render()
{
    if (triangleIndexCount == 0 && lineIndexCount == 0)
        return;

    shader.Use();
    shader.SetMat4("uViewProjection", viewProjection);
    shader.SetMat4("uModel", glm::mat4(1.0f));

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (triangleIndexCount > 0)
    {
        glBindVertexArray(triangleVAO);
        glDrawElements(GL_TRIANGLES, triangleIndexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }

    if (lineIndexCount > 0)
    {
        glBindVertexArray(lineVAO);
        glDrawElements(GL_LINES, lineIndexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

void AnalysisRenderer::Shutdown()
{
    shader.Delete();
    if (triangleVBO)
        glDeleteBuffers(1, &triangleVBO);
    if (triangleIBO)
        glDeleteBuffers(1, &triangleIBO);
    if (triangleVAO)
        glDeleteVertexArrays(1, &triangleVAO);
    if (lineVBO)
        glDeleteBuffers(1, &lineVBO);
    if (lineIBO)
        glDeleteBuffers(1, &lineIBO);
    if (lineVAO)
        glDeleteVertexArrays(1, &lineVAO);

    triangleVAO = triangleVBO = triangleIBO = 0;
    lineVAO = lineVBO = lineIBO = 0;
    triangleIndexCount = lineIndexCount = 0;
}
