#include "SceneRenderer.hpp"
#include "utils/log.hpp"

SceneRenderer::SceneRenderer(GLFWwindow *w) : renderer(w)
{
}

void SceneRenderer::UpdateFromRenderBuffer(const Scene &scene)
{
    GLint viewPort[4];
    glGetIntegerv(GL_VIEWPORT, viewPort);

    if (currentMode == RenderMode::WIREFRAME || currentMode == RenderMode::WIREFRAME_ON_SOLID)
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;

        wireframe.Generate(scene, renderBuffer, vertices, indices, viewPort);
        renderer.UploadLineMesh(vertices, indices);
    }

    if (currentMode == RenderMode::SOLID || currentMode == RenderMode::WIREFRAME_ON_SOLID)
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;

        patch.Generate(scene, renderBuffer, vertices, indices, viewPort);

        renderer.UploadTriangleMesh(vertices, indices);
    }
}

void SceneRenderer::SetCamera(Camera &camera)
{
    renderer.SetViewMatrix(camera.GetViewMatrix());
    renderer.SetProjectionMatrix(camera.GetProjectionMatrix());
    renderer.SetModelMatrix(glm::mat4(1.0f));
}

void SceneRenderer::Render()
{
    renderer.BeginFrame();
    renderer.Clear(Color::GetBase());
    if (currentMode == RenderMode::WIREFRAME ||
        currentMode == RenderMode::WIREFRAME_ON_SOLID)
    {
        renderer.DrawLines();
    }

    if (currentMode == RenderMode::SOLID ||
        currentMode == RenderMode::WIREFRAME_ON_SOLID)
    {
        renderer.DrawTriangles();
    }
    renderer.EndFrame();
}

void SceneRenderer::SetRenderMode(RenderMode mode)
{
    currentMode = mode;
}

void SceneRenderer::Shutdown()
{
    renderer.Shutdown();
}

void SceneRenderer::AddForm(uint32_t id)
{
    renderBuffer.AddForm(id);
}
