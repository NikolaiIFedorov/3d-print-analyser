#include "OpenGLRenderer.hpp"
#include "utils/log.hpp"
#include "rendering/color.hpp"
#include "ViewportDepthExperiments.hpp"
#include "RenderingExperiments.hpp"

namespace
{
[[nodiscard]] GLenum DepthComparePass()
{
    return RenderingExperiments::kReverseZDepth ? GL_GEQUAL : GL_LEQUAL;
}

[[nodiscard]] float LineShaderWireZNudgeNdc()
{
    if (ViewportDepthExperiments::IsNoWireZBias())
        return 0.0f;
    const float n = 1.0e-6f * RenderingExperiments::kWireframeClipZNudgeScale;
    return RenderingExperiments::kReverseZDepth ? -n : n;
}
} // namespace

OpenGLRenderer::~OpenGLRenderer()
{
    Shutdown();
}

OpenGLRenderer::OpenGLRenderer(OpenGLRenderer &&other) noexcept
    : window(other.window),
      glContext(other.glContext),
      triangleVAO(other.triangleVAO), triangleVBO(other.triangleVBO), triangleIBO(other.triangleIBO),
      triangleIndexCount(other.triangleIndexCount), triangleVertexCount(other.triangleVertexCount),
      lineVAO(other.lineVAO), lineVBO(other.lineVBO), lineIBO(other.lineIBO), lineIndexCount(other.lineIndexCount),
      lineVertexCount(other.lineVertexCount),
      pickHighlightVAO(other.pickHighlightVAO), pickHighlightVBO(other.pickHighlightVBO),
      pickHighlightIBO(other.pickHighlightIBO), pickHighlightIndexCount(other.pickHighlightIndexCount),
      pickHighlightLineVAO(other.pickHighlightLineVAO), pickHighlightLineVBO(other.pickHighlightLineVBO),
      pickHighlightLineIBO(other.pickHighlightLineIBO), pickHighlightLineIndexCount(other.pickHighlightLineIndexCount),
      viewMatrix(other.viewMatrix), projectionMatrix(other.projectionMatrix), modelMatrix(other.modelMatrix),
      shader(std::move(other.shader)),
      lineShader(std::move(other.lineShader))
{
    other.window = nullptr;
    other.glContext = nullptr;
    other.triangleVAO = other.triangleVBO = other.triangleIBO = 0;
    other.lineVAO = other.lineVBO = other.lineIBO = 0;
    other.pickHighlightVAO = other.pickHighlightVBO = other.pickHighlightIBO = 0;
    other.pickHighlightLineVAO = other.pickHighlightLineVBO = other.pickHighlightLineIBO = 0;
    other.triangleIndexCount = other.triangleVertexCount = other.lineIndexCount = other.lineVertexCount =
        other.pickHighlightIndexCount = other.pickHighlightLineIndexCount = 0;
    other.triangleVertexCapacity = other.triangleIndexCapacity = 0;
    other.lineVertexCapacity = other.lineIndexCapacity = 0;
}

OpenGLRenderer &OpenGLRenderer::operator=(OpenGLRenderer &&other) noexcept
{
    if (this != &other)
    {
        Shutdown();
        window = other.window;
        glContext = other.glContext;
        triangleVAO = other.triangleVAO;
        triangleVBO = other.triangleVBO;
        triangleIBO = other.triangleIBO;
        triangleIndexCount = other.triangleIndexCount;
        triangleVertexCount = other.triangleVertexCount;
        lineVAO = other.lineVAO;
        lineVBO = other.lineVBO;
        lineIBO = other.lineIBO;
        lineIndexCount = other.lineIndexCount;
        lineVertexCount = other.lineVertexCount;
        pickHighlightVAO = other.pickHighlightVAO;
        pickHighlightVBO = other.pickHighlightVBO;
        pickHighlightIBO = other.pickHighlightIBO;
        pickHighlightIndexCount = other.pickHighlightIndexCount;
        pickHighlightLineVAO = other.pickHighlightLineVAO;
        pickHighlightLineVBO = other.pickHighlightLineVBO;
        pickHighlightLineIBO = other.pickHighlightLineIBO;
        pickHighlightLineIndexCount = other.pickHighlightLineIndexCount;
        viewMatrix = other.viewMatrix;
        projectionMatrix = other.projectionMatrix;
        modelMatrix = other.modelMatrix;
        shader = std::move(other.shader);
        lineShader = std::move(other.lineShader);
        other.window = nullptr;
        other.glContext = nullptr;
        other.triangleVAO = other.triangleVBO = other.triangleIBO = 0;
        other.lineVAO = other.lineVBO = other.lineIBO = 0;
        other.pickHighlightVAO = other.pickHighlightVBO = other.pickHighlightIBO = 0;
        other.pickHighlightLineVAO = other.pickHighlightLineVBO = other.pickHighlightLineIBO = 0;
        other.triangleIndexCount = other.triangleVertexCount = other.lineIndexCount = other.lineVertexCount =
            other.pickHighlightIndexCount = other.pickHighlightLineIndexCount = 0;
        other.triangleVertexCapacity = other.triangleIndexCapacity = 0;
        other.lineVertexCapacity = other.lineIndexCapacity = 0;
    }
    return *this;
}

OpenGLRenderer::OpenGLRenderer(SDL_Window *windowHandle)
{
    if (windowHandle == nullptr)
    {
        LOG_FALSE("WindowHandle is null");
        return;
    }

    window = windowHandle;

    glContext = SDL_GL_GetCurrentContext();
    SDL_GL_MakeCurrent(windowHandle, glContext);
    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress))
    {
        GetGLError();
        return;
    }

    GLint sampleBuffers = 0;
    GLint samples = 0;
    glGetIntegerv(GL_SAMPLE_BUFFERS, &sampleBuffers);
    glGetIntegerv(GL_SAMPLES, &samples);
    LOG_DESC("Multisample: GL_SAMPLE_BUFFERS=" + std::to_string(sampleBuffers) +
             " GL_SAMPLES=" + std::to_string(samples));
    if (RenderingExperiments::kReverseZDepth)
    {
        LOG_DESC("Depth mode: reverse-Z");
    }
    else
    {
        LOG_DESC("Depth mode: forward");
    }

    if (RenderingExperiments::kGlFramebufferMsaaSamples > 0)
        glEnable(GL_MULTISAMPLE);
    else
        glDisable(GL_MULTISAMPLE);

    glEnable(GL_DEPTH_TEST);
    if (RenderingExperiments::kReverseZDepth)
    {
        glClearDepth(0.0);
        glDepthFunc(GL_GEQUAL);
    }
    else
    {
        glClearDepth(1.0);
        glDepthFunc(GL_LEQUAL);
    }

    glDisable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    if (!InitializeShaders())
    {
        LOG_FALSE("Failed to initialize shaders");
        return;
    }

    glGenVertexArrays(1, &triangleVAO);
    glGenVertexArrays(1, &lineVAO);

    LOG_DESC("OpenGL Version: " + std::string((const char *)glGetString(GL_VERSION)));
    LOG_DESC("GLSL Version: " + std::string((const char *)glGetString(GL_SHADING_LANGUAGE_VERSION)));
    LOG_DESC("Renderer: " + std::string((const char *)glGetString(GL_RENDERER)));

    GLint depthBits;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depthBits);
    LOG_DESC("Depth buffer bits: " + std::to_string(depthBits));

    LOG_VOID("Initialized renderer");

    GetGLError();
}

bool OpenGLRenderer::InitializeShaders()
{
    if (!shader.LoadFromFiles("shaders/basic.vert",
                              "shaders/basic.frag"))
        return false;

    if (!lineShader.LoadFromFiles("shaders/line.vert",
                                  "shaders/line.geom",
                                  "shaders/line.frag"))
        return false;

    return true;
}

void OpenGLRenderer::Shutdown()
{
    shader.Delete();
    lineShader.Delete();

    if (triangleVBO)
        glDeleteBuffers(1, &triangleVBO);
    if (triangleVAO)
        glDeleteVertexArrays(1, &triangleVAO);
    if (triangleIBO)
        glDeleteBuffers(1, &triangleIBO);

    if (pickHighlightVBO)
        glDeleteBuffers(1, &pickHighlightVBO);
    if (pickHighlightVAO)
        glDeleteVertexArrays(1, &pickHighlightVAO);
    if (pickHighlightIBO)
        glDeleteBuffers(1, &pickHighlightIBO);
    pickHighlightVAO = pickHighlightVBO = pickHighlightIBO = 0;
    pickHighlightIndexCount = 0;

    if (pickHighlightLineVBO)
        glDeleteBuffers(1, &pickHighlightLineVBO);
    if (pickHighlightLineVAO)
        glDeleteVertexArrays(1, &pickHighlightLineVAO);
    if (pickHighlightLineIBO)
        glDeleteBuffers(1, &pickHighlightLineIBO);
    pickHighlightLineVAO = pickHighlightLineVBO = pickHighlightLineIBO = 0;
    pickHighlightLineIndexCount = 0;

    if (lineVBO)
        glDeleteBuffers(1, &lineVBO);
    if (lineVAO)
        glDeleteVertexArrays(1, &lineVAO);
    if (lineIBO)
        glDeleteBuffers(1, &lineIBO);

    triangleVertexCapacity = triangleIndexCapacity = 0;
    lineVertexCapacity = lineIndexCapacity = 0;
}

void OpenGLRenderer::EndFrame()
{
    SDL_GL_SwapWindow(window);

    GetGLError();

    LOG_VOID("Rendering frame with: #indices = " + Log::NumToStr(triangleIndexCount));
}

void OpenGLRenderer::Clear(const glm::vec3 &color)
{
    glClearColor(color.r, color.g, color.b, 1.0f);
    glClearDepth(RenderingExperiments::kReverseZDepth ? 0.0 : 1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void OpenGLRenderer::SetViewMatrix(const glm::mat4 &view)
{
    viewMatrix = view;
}

void OpenGLRenderer::SetProjectionMatrix(const glm::mat4 &projection)
{
    projectionMatrix = projection;
}

void OpenGLRenderer::SetModelMatrix(const glm::mat4 &model)
{
    modelMatrix = model;
}

void OpenGLRenderer::SetViewPos(const glm::vec3 &pos)
{
    viewPos = pos;
    // Headlight: light comes from the camera direction
    lightDir = glm::normalize(viewPos);
}

void OpenGLRenderer::UploadTriangleMesh(const std::vector<Vertex> &vertices,
                                        const std::vector<uint32_t> &indices)
{
    triangleIndexCount = static_cast<uint32_t>(indices.size());
    triangleVertexCount = static_cast<uint32_t>(vertices.size());
    triangleVertexCapacity = vertices.size();
    triangleIndexCapacity = indices.size();

    glBindVertexArray(triangleVAO);

    if (triangleVBO == 0)
    {
        glGenBuffers(1, &triangleVBO);
    }

    glBindBuffer(GL_ARRAY_BUFFER, triangleVBO);

    glBufferData(GL_ARRAY_BUFFER,
                 vertices.size() * sizeof(Vertex),
                 vertices.data(),
                 GL_DYNAMIC_DRAW);

    if (triangleIBO == 0)
    {
        glGenBuffers(1, &triangleIBO);
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, triangleIBO);

    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 indices.size() * sizeof(uint32_t),
                 indices.data(),
                 GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          sizeof(Vertex),
                          (void *)offsetof(Vertex, position));
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
                          sizeof(Vertex),
                          (void *)offsetof(Vertex, color));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE,
                          sizeof(Vertex),
                          (void *)offsetof(Vertex, normal));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);
    GetGLError();
}

bool OpenGLRenderer::UpdateTriangleMeshSubData(const std::vector<Vertex> &vertices, size_t vertexOffset,
                                               const std::vector<uint32_t> &indices, size_t indexOffset)
{
    if (triangleVBO == 0 || triangleIBO == 0)
        return false;
    if (vertexOffset + vertices.size() > triangleVertexCapacity)
        return false;
    if (indexOffset + indices.size() > triangleIndexCapacity)
        return false;

    glBindVertexArray(triangleVAO);
    glBindBuffer(GL_ARRAY_BUFFER, triangleVBO);
    glBufferSubData(GL_ARRAY_BUFFER,
                    static_cast<GLintptr>(vertexOffset * sizeof(Vertex)),
                    static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
                    vertices.empty() ? nullptr : vertices.data());

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, triangleIBO);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER,
                    static_cast<GLintptr>(indexOffset * sizeof(uint32_t)),
                    static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)),
                    indices.empty() ? nullptr : indices.data());

    glBindVertexArray(0);
    GetGLError();
    return true;
}

void OpenGLRenderer::UploadLineMesh(const std::vector<Vertex> &vertices,
                                    const std::vector<uint32_t> &indices)
{
    lineIndexCount = static_cast<uint32_t>(indices.size());
    lineVertexCount = static_cast<uint32_t>(vertices.size());
    lineVertexCapacity = vertices.size();
    lineIndexCapacity = indices.size();

    if (lineVAO == 0)
        glGenVertexArrays(1, &lineVAO);

    glBindVertexArray(lineVAO);

    if (lineVBO == 0)
        glGenBuffers(1, &lineVBO);

    glBindBuffer(GL_ARRAY_BUFFER, lineVBO);
    glBufferData(GL_ARRAY_BUFFER,
                 vertices.size() * sizeof(Vertex),
                 vertices.data(),
                 GL_DYNAMIC_DRAW);

    if (lineIBO == 0)
        glGenBuffers(1, &lineIBO);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lineIBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 indices.size() * sizeof(uint32_t),
                 indices.data(),
                 GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          sizeof(Vertex),
                          (void *)offsetof(Vertex, position));
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
                          sizeof(Vertex),
                          (void *)offsetof(Vertex, color));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE,
                          sizeof(Vertex),
                          (void *)offsetof(Vertex, normal));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    GetGLError();
}

bool OpenGLRenderer::UpdateLineMeshSubData(const std::vector<Vertex> &vertices, size_t vertexOffset,
                                           const std::vector<uint32_t> &indices, size_t indexOffset)
{
    if (lineVBO == 0 || lineIBO == 0)
        return false;
    if (vertexOffset + vertices.size() > lineVertexCapacity)
        return false;
    if (indexOffset + indices.size() > lineIndexCapacity)
        return false;

    glBindVertexArray(lineVAO);
    glBindBuffer(GL_ARRAY_BUFFER, lineVBO);
    glBufferSubData(GL_ARRAY_BUFFER,
                    static_cast<GLintptr>(vertexOffset * sizeof(Vertex)),
                    static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
                    vertices.empty() ? nullptr : vertices.data());

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lineIBO);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER,
                    static_cast<GLintptr>(indexOffset * sizeof(uint32_t)),
                    static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)),
                    indices.empty() ? nullptr : indices.data());

    glBindVertexArray(0);
    GetGLError();
    return true;
}

void OpenGLRenderer::UploadPickHighlightLineMesh(const std::vector<Vertex> &vertices,
                                                 const std::vector<uint32_t> &indices)
{
    pickHighlightLineIndexCount = static_cast<uint32_t>(indices.size());

    if (pickHighlightLineVAO == 0)
        glGenVertexArrays(1, &pickHighlightLineVAO);

    glBindVertexArray(pickHighlightLineVAO);

    if (pickHighlightLineVBO == 0)
        glGenBuffers(1, &pickHighlightLineVBO);

    glBindBuffer(GL_ARRAY_BUFFER, pickHighlightLineVBO);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
                 vertices.empty() ? nullptr : vertices.data(),
                 GL_DYNAMIC_DRAW);

    if (pickHighlightLineIBO == 0)
        glGenBuffers(1, &pickHighlightLineIBO);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, pickHighlightLineIBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)),
                 indices.empty() ? nullptr : indices.data(),
                 GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, position));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, color));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, normal));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    GetGLError();
}

void OpenGLRenderer::DrawTriangles()
{
    LOG_DESC("Drawing triangles with index count: " + std::to_string(triangleIndexCount));
    if (triangleIndexCount == 0)
        return;

    if (RenderingExperiments::kDepthPrepassOpaquePatches)
        DrawTrianglesPass(false);
    DrawTrianglesPass(true);
}

void OpenGLRenderer::DrawTrianglesPass(bool writeColor)
{
    shader.Use();

    glm::mat4 viewProj = projectionMatrix * viewMatrix;
    shader.SetMat4("uViewProjection", viewProj);

    shader.SetMat4("uModel", modelMatrix);

    // Light from positive XYZ corner, matching the axis indicator colors
    shader.SetVec3("uLightDir", glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)));
    shader.SetVec3("uViewPos", viewPos);
    // Keep directional lighting readable without washing bright faces enough to hide wire edges.
    shader.SetFloat("uBrightenAmount", 0.75f);
    shader.SetFloat("uBlueMin", 0.0f);
    shader.SetFloat("uBlueMax", Color::GetBase().b * 10.0f);
    shader.SetFloat("uBlueNear", 0.0f);
    shader.SetFloat("uBlueFar", Color::GRID_EXTENT);
    shader.SetFloat("uGridPlaneFade", 0.0f);
    shader.SetFloat("uGridLodStep", 1.0f);
    shader.SetFloat("uClipZBiasW", RenderingExperiments::ClipZBiasSceneMeshW());
    shader.SetFloat("uLightingEnabled", 1.0f);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(DepthComparePass());
    glDepthMask(GL_TRUE);

    if (writeColor)
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    else
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    glBindVertexArray(triangleVAO);
    glDrawElements(GL_TRIANGLES, triangleIndexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    GetGLError();
}

void OpenGLRenderer::UploadPickHighlightMesh(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices)
{
    pickHighlightIndexCount = static_cast<uint32_t>(indices.size());

    if (pickHighlightVAO == 0)
        glGenVertexArrays(1, &pickHighlightVAO);

    glBindVertexArray(pickHighlightVAO);

    if (pickHighlightVBO == 0)
        glGenBuffers(1, &pickHighlightVBO);

    glBindBuffer(GL_ARRAY_BUFFER, pickHighlightVBO);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
                 vertices.empty() ? nullptr : vertices.data(),
                 GL_DYNAMIC_DRAW);

    if (pickHighlightIBO == 0)
        glGenBuffers(1, &pickHighlightIBO);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, pickHighlightIBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)),
                 indices.empty() ? nullptr : indices.data(),
                 GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, position));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, color));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, normal));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);
    GetGLError();
}

void OpenGLRenderer::DrawPickHighlight()
{
    if (pickHighlightIndexCount == 0)
        return;

    shader.Use();

    glm::mat4 viewProj = projectionMatrix * viewMatrix;
    shader.SetMat4("uViewProjection", viewProj);
    shader.SetMat4("uModel", modelMatrix);
    shader.SetVec3("uLightDir", glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)));
    shader.SetVec3("uViewPos", viewPos);
    shader.SetFloat("uBrightenAmount", 0.85f);
    shader.SetFloat("uBlueMin", 0.0f);
    shader.SetFloat("uBlueMax", Color::GetBase().b * 10.0f);
    shader.SetFloat("uBlueNear", 0.0f);
    shader.SetFloat("uBlueFar", Color::GRID_EXTENT);
    shader.SetFloat("uGridPlaneFade", 0.0f);
    shader.SetFloat("uGridLodStep", 1.0f);
    shader.SetFloat("uClipZBiasW", 0.0f);
    shader.SetFloat("uLightingEnabled", 1.0f);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(DepthComparePass());
    // Highlight fill should not overwrite scene depth; otherwise subsequent wireframe edges can
    // fail depth and disappear on selected faces.
    glDepthMask(GL_FALSE);

    const bool skipPickPolygonOffset = ViewportDepthExperiments::IsNoPickPolygonOffset() ||
                                       RenderingExperiments::kPickHighlightNoPolygonOffset;
    if (!skipPickPolygonOffset)
    {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -1.0f);
    }

    glBindVertexArray(pickHighlightVAO);
    glDrawElements(GL_TRIANGLES, pickHighlightIndexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

    if (!skipPickPolygonOffset)
        glDisable(GL_POLYGON_OFFSET_FILL);

    glDepthMask(GL_TRUE);

    GetGLError();
}

void OpenGLRenderer::DrawPickHighlightLines(float pixelWidth)
{
    if (pickHighlightLineIndexCount == 0)
        return;

    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);

    lineShader.Use();
    lineShader.SetMat4("uViewProjection", projectionMatrix * viewMatrix);
    lineShader.SetMat4("uModel", modelMatrix);
    lineShader.SetVec2("uViewportSize", glm::vec2(viewport[2], viewport[3]));
    lineShader.SetFloat("uLineWidth", pixelWidth);
    lineShader.SetFloat("uWireZNudgeNdc", LineShaderWireZNudgeNdc());
    lineShader.SetFloat("uClipZBiasW", 0.0f);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(DepthComparePass());
    glDepthMask(RenderingExperiments::kLineDrawsOmitDepthWrite ? GL_FALSE : GL_TRUE);

    if (RenderingExperiments::kWireframeLinePolygonOffsetDeeper)
    {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(0.0f, 1.5f);
    }

    glBindVertexArray(pickHighlightLineVAO);
    glDrawElements(GL_LINES, pickHighlightLineIndexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

    if (RenderingExperiments::kWireframeLinePolygonOffsetDeeper)
        glDisable(GL_POLYGON_OFFSET_FILL);

    glDepthMask(GL_TRUE);

    GetGLError();
}

void OpenGLRenderer::DrawLines()
{
    LOG_DESC("Drawing lines with index count: " + std::to_string(lineIndexCount));
    if (lineIndexCount == 0)
        return;

    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);

    lineShader.Use();
    lineShader.SetMat4("uViewProjection", projectionMatrix * viewMatrix);
    lineShader.SetMat4("uModel", modelMatrix);
    lineShader.SetVec2("uViewportSize", glm::vec2(viewport[2], viewport[3]));
    lineShader.SetFloat("uLineWidth", lineWidth);
    lineShader.SetFloat("uWireZNudgeNdc", LineShaderWireZNudgeNdc() * wireframeDepthNudgeScale);
    lineShader.SetFloat("uClipZBiasW", RenderingExperiments::ClipZBiasSceneMeshW());

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(DepthComparePass());
    glDepthMask(RenderingExperiments::kLineDrawsOmitDepthWrite ? GL_FALSE : GL_TRUE);

    if (RenderingExperiments::kWireframeLinePolygonOffsetDeeper)
    {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(0.0f, 1.5f * wireframeDepthNudgeScale);
    }

    glBindVertexArray(lineVAO);
    glDrawElements(GL_LINES, lineIndexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

    if (RenderingExperiments::kWireframeLinePolygonOffsetDeeper)
        glDisable(GL_POLYGON_OFFSET_FILL);

    glDepthMask(GL_TRUE);

    GetGLError();
}

void OpenGLRenderer::SetWireFrameMode(bool enabled)
{
    if (enabled)
    {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }
    else
    {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    GetGLError();
}

bool OpenGLRenderer::GetGLError(const std::source_location &loc)
{
    GLenum ERROR = glGetError();
    std::string errorStr;

    switch (ERROR)
    {
    case GL_INVALID_ENUM:
        errorStr = "INVALID_ENUM";
        break;
    case GL_INVALID_VALUE:
        errorStr = "INVALID_VALUE";
        break;
    case GL_INVALID_OPERATION:
        errorStr = "INVALID_OPERATION";
        break;
    case GL_OUT_OF_MEMORY:
        errorStr = "OUT_OF_MEMORY";
        break;
    case GL_INVALID_FRAMEBUFFER_OPERATION:
        errorStr = "INVALID_FRAMEBUFFER_OPERATION";
        break;
    default:
        errorStr = "";
        break;
    }

    if (errorStr != "")
        return LOG_MSG("GLerror: " + errorStr, loc, Level::WARN, BoolType::FALSE);

    return true;
}