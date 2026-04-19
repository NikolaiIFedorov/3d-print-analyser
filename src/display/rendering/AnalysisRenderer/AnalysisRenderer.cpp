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
      lineShader(std::move(other.lineShader)),
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
        lineShader = std::move(other.lineShader);
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
    if (!shader.LoadFromFiles("shaders/analysis.vert", "shaders/analysis.frag"))
        return false;

    if (!lineShader.LoadFromFiles("shaders/analysis_line.vert",
                                  "shaders/analysis_line.geom",
                                  "shaders/analysis_line.frag"))
        return false;

    return true;
}

void AnalysisRenderer::SetCamera(Camera &camera)
{
    viewProjection = camera.GetProjectionMatrix() * camera.GetViewMatrix();
}

void AnalysisRenderer::Update(Scene *scene, const AnalysisResults &results)
{
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

    // Ensure consistent winding for earcut: outer loop CCW, inner loops CW
    if (polygon.size() > 1)
    {
        auto signedArea2D = [](const std::vector<Point2D> &loop) -> double
        {
            double area = 0.0;
            for (size_t j = 0; j < loop.size(); j++)
            {
                size_t k = (j + 1) % loop.size();
                area += loop[j][0] * loop[k][1] - loop[k][0] * loop[j][1];
            }
            return area;
        };

        if (signedArea2D(polygon[0]) < 0)
        {
            std::reverse(polygon[0].begin(), polygon[0].end());
            std::reverse(allLoopPositions[0].begin(), allLoopPositions[0].end());
        }

        for (size_t li = 1; li < polygon.size(); li++)
        {
            if (signedArea2D(polygon[li]) > 0)
            {
                std::reverse(polygon[li].begin(), polygon[li].end());
                std::reverse(allLoopPositions[li].begin(), allLoopPositions[li].end());
            }
        }
    }

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

static void TriangulateClipBoundary(const Face *face,
                                    const std::vector<glm::dvec3> &boundary,
                                    const glm::vec4 &color,
                                    std::vector<AnalysisVertex> &vertices,
                                    std::vector<uint32_t> &indices)
{
    if (boundary.size() < 3)
        return;

    glm::dvec3 faceNormal = face->GetSurface().GetNormal();
    glm::dvec3 uAxis, vAxis;
    CreatePlaneCoordinateSystem(faceNormal, uAxis, vAxis);

    glm::dvec3 origin = boundary[0];

    using Point2D = std::array<double, 2>;
    std::vector<Point2D> loop2D;
    for (const auto &pt : boundary)
    {
        glm::dvec2 p2d = ProjectPointToPlane(pt, origin, uAxis, vAxis);
        loop2D.push_back({p2d.x, p2d.y});
    }

    // Ensure CCW winding for earcut
    double area = 0.0;
    for (size_t j = 0; j < loop2D.size(); j++)
    {
        size_t k = (j + 1) % loop2D.size();
        area += loop2D[j][0] * loop2D[k][1] - loop2D[k][0] * loop2D[j][1];
    }

    std::vector<glm::dvec3> ordered = boundary;
    if (area < 0)
    {
        std::reverse(loop2D.begin(), loop2D.end());
        std::reverse(ordered.begin(), ordered.end());
    }

    std::vector<std::vector<Point2D>> polygon;
    polygon.push_back(loop2D);

    auto triIndices = mapbox::earcut<uint32_t>(polygon);
    if (triIndices.empty())
        return;

    uint32_t base = vertices.size();
    for (const auto &pt : ordered)
        vertices.push_back({glm::vec3(pt), color});

    for (uint32_t idx : triIndices)
        indices.push_back(base + idx);
}

// Clip a 3D loop against a Z half-plane. keep vertices where inside(v) is true.
static std::vector<glm::dvec3> ClipLoopByZ(const std::vector<glm::dvec3> &loop,
                                           double zPlane, bool keepAbove)
{
    std::vector<glm::dvec3> out;
    if (loop.empty())
        return out;

    auto inside = [&](const glm::dvec3 &p)
    {
        return keepAbove ? p.z >= zPlane - 1e-10 : p.z <= zPlane + 1e-10;
    };

    for (size_t i = 0; i < loop.size(); i++)
    {
        const glm::dvec3 &curr = loop[i];
        const glm::dvec3 &next = loop[(i + 1) % loop.size()];
        bool currIn = inside(curr);
        bool nextIn = inside(next);

        if (currIn)
            out.push_back(curr);

        if (currIn != nextIn)
        {
            double dz = next.z - curr.z;
            if (std::abs(dz) > 1e-14)
            {
                double t = (zPlane - curr.z) / dz;
                out.push_back(curr + t * (next - curr));
            }
        }
    }
    return out;
}

static void TriangulateFaceZClipped(const Face *face, const ZBounds &zb,
                                    const glm::vec4 &color,
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

        // Clip against zMin and zMax
        loopPositions = ClipLoopByZ(loopPositions, zb.zMin, true);
        loopPositions = ClipLoopByZ(loopPositions, zb.zMax, false);

        if (loopPositions.size() < 3)
            continue;

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

    if (polygon.size() > 1)
    {
        auto signedArea2D = [](const std::vector<Point2D> &loop) -> double
        {
            double area = 0.0;
            for (size_t j = 0; j < loop.size(); j++)
            {
                size_t k = (j + 1) % loop.size();
                area += loop[j][0] * loop[k][1] - loop[k][0] * loop[j][1];
            }
            return area;
        };

        if (signedArea2D(polygon[0]) < 0)
        {
            std::reverse(polygon[0].begin(), polygon[0].end());
            std::reverse(allLoopPositions[0].begin(), allLoopPositions[0].end());
        }

        for (size_t li = 1; li < polygon.size(); li++)
        {
            if (signedArea2D(polygon[li]) > 0)
            {
                std::reverse(polygon[li].begin(), polygon[li].end());
                std::reverse(allLoopPositions[li].begin(), allLoopPositions[li].end());
            }
        }
    }

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
        if (flaw == FaceFlawKind::NONE)
            continue;
        TriangulateFace(face, Color::GetFace(flaw), vertices, indices);
    }

    for (const auto &[solid, faceFlaws] : results.faceFlawRanges)
    {
        for (const auto &ff : faceFlaws)
        {
            glm::vec4 color = Color::GetFace(ff.flaw);
            if (!ff.clipBoundary.empty())
                TriangulateClipBoundary(ff.face, ff.clipBoundary, color, vertices, indices);
            else
                TriangulateFaceZClipped(ff.face, ff.bounds, color, vertices, indices);
        }
    }

    // Generate vertical bridge surfaces connecting paired faces
    for (const auto &[solid, bridges] : results.bridgeSurfaces)
    {
        for (const auto &bs : bridges)
        {
            if (bs.boundary.size() < 3)
                continue;

            glm::vec4 color = Color::GetFace(bs.flaw);

            // Compute a normal from the boundary to build a 2D coordinate system
            glm::dvec3 edge1 = bs.boundary[1] - bs.boundary[0];
            glm::dvec3 edge2 = bs.boundary.back() - bs.boundary[0];
            glm::dvec3 normal = glm::normalize(glm::cross(edge1, edge2));

            if (glm::length(normal) < 1e-10)
                continue;

            glm::dvec3 uAxis, vAxis;
            CreatePlaneCoordinateSystem(normal, uAxis, vAxis);
            glm::dvec3 origin = bs.boundary[0];

            using Point2D = std::array<double, 2>;
            std::vector<Point2D> loop2D;
            for (const auto &pt : bs.boundary)
            {
                glm::dvec3 rel = pt - origin;
                loop2D.push_back({glm::dot(rel, uAxis), glm::dot(rel, vAxis)});
            }

            // Ensure CCW winding
            double area = 0.0;
            for (size_t j = 0; j < loop2D.size(); j++)
            {
                size_t k = (j + 1) % loop2D.size();
                area += loop2D[j][0] * loop2D[k][1] - loop2D[k][0] * loop2D[j][1];
            }

            std::vector<glm::dvec3> ordered = bs.boundary;
            if (area < 0)
            {
                std::reverse(loop2D.begin(), loop2D.end());
                std::reverse(ordered.begin(), ordered.end());
            }

            std::vector<std::vector<Point2D>> polygon;
            polygon.push_back(loop2D);

            auto triIndices = mapbox::earcut<uint32_t>(polygon);
            if (triIndices.empty())
                continue;

            uint32_t base = vertices.size();
            for (const auto &pt : ordered)
                vertices.push_back({glm::vec3(pt), color});

            for (uint32_t idx : triIndices)
                indices.push_back(base + idx);
        }
    }
}

void AnalysisRenderer::GenerateLayerLines(const AnalysisResults &results,
                                          std::vector<AnalysisVertex> &vertices,
                                          std::vector<uint32_t> &indices) const
{
    for (const auto &[solid, edgeFlaws] : results.edgeFlaws)
    {
        for (const auto &ef : edgeFlaws)
        {
            glm::vec4 color = Color::GetEdge(ef.flaw);
            uint32_t base = vertices.size();
            vertices.push_back({glm::vec3(ef.edge->startPoint->position), color});
            vertices.push_back({glm::vec3(ef.edge->endPoint->position), color});
            indices.push_back(base);
            indices.push_back(base + 1);
        }
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
    if (lineIndexCount == 0)
        return;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);

    lineShader.Use();
    lineShader.SetMat4("uViewProjection", viewProjection);
    lineShader.SetMat4("uModel", glm::mat4(1.0f));
    lineShader.SetVec2("uViewportSize", glm::vec2(viewport[2], viewport[3]));
    lineShader.SetFloat("uLineWidth", lineWidth);

    glBindVertexArray(lineVAO);
    glDrawElements(GL_LINES, lineIndexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}

void AnalysisRenderer::Shutdown()
{
    shader.Delete();
    lineShader.Delete();
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
