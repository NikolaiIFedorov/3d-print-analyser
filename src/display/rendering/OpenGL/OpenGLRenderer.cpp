#include "OpenGLRenderer.hpp"
#include "utils/log.hpp"

glm::mat4 test;

OpenGLRenderer::~OpenGLRenderer()
{
    Shutdown();
}

OpenGLRenderer::OpenGLRenderer(GLFWwindow *window)
{
    Initialize(window);
}

bool OpenGLRenderer::Initialize(GLFWwindow *windowHandle)
{
    if (windowHandle == nullptr)
    {
        return LOG_FALSE("WindowHandle is null");
    }

    window = windowHandle;

    glfwMakeContextCurrent(windowHandle);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        GetGLError();
        return false;
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glClearDepth(1.0);

    glDisable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    if (!InitializeShaders())
    {
        return false;
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
    return true;
}

bool OpenGLRenderer::InitializeShaders()
{
    return shader.LoadFromFiles("shaders/basic.vert",
                                "shaders/basic.frag");
}

void OpenGLRenderer::Shutdown()
{
    shader.Delete();

    if (triangleVBO)
        glDeleteBuffers(1, &triangleVBO);
    if (triangleVAO)
        glDeleteVertexArrays(1, &triangleVAO);
    if (triangleIBO)
        glDeleteBuffers(1, &triangleIBO);

    if (lineVBO)
        glDeleteBuffers(1, &lineVBO);
    if (lineVAO)
        glDeleteVertexArrays(1, &lineVAO);
    if (lineIBO)
        glDeleteBuffers(1, &lineIBO);
}

void OpenGLRenderer::BeginFrame()
{
}

void OpenGLRenderer::EndFrame()
{
    glfwSwapBuffers(window);
    GetGLError();
    LOG_VOID("Rendering frame with: #indices = " + Log::NumToStr(triangleIndexCount));
}

void OpenGLRenderer::Clear(const glm::vec3 &color)
{
    glClearColor(color.r, color.g, color.b, 1.0f);
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

void OpenGLRenderer::UploadTriangleMesh(const std::vector<Vertex> &vertices,
                                        const std::vector<uint32_t> &indices)
{
    triangleIndexCount = static_cast<uint32_t>(indices.size());

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

    glBindVertexArray(0);
    GetGLError();
}

void OpenGLRenderer::UploadLineMesh(const std::vector<Vertex> &vertices,
                                    const std::vector<uint32_t> &indices)
{
    lineIndexCount = indices.size();

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

    glBindVertexArray(0);
    GetGLError();
}

void OpenGLRenderer::DrawTriangles()
{
    if (triangleIndexCount == 0)
    {
        return;
    }

    shader.Use();

    GLint currentProgram;
    glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
    glm::mat4 viewProj = projectionMatrix * viewMatrix;
    shader.SetMat4("uViewProjection", viewProj);
    test = viewProj;

    shader.SetMat4("uModel", modelMatrix);

    // Force depth state
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);

    // Push triangles slightly back so edges render on top
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0f, 1.0f);

    glBindVertexArray(triangleVAO);

    glDrawElements(GL_TRIANGLES, triangleIndexCount, GL_UNSIGNED_INT, 0);

    glBindVertexArray(0);

    glDisable(GL_POLYGON_OFFSET_FILL);
    GetGLError();
}

void OpenGLRenderer::DrawLines()
{
    if (lineIndexCount == 0)
        return;

    shader.Use();
    shader.SetMat4("uViewProjection", projectionMatrix * viewMatrix);
    shader.SetMat4("uModel", modelMatrix);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);

    glBindVertexArray(lineVAO);
    glDrawElements(GL_LINES, lineIndexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

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