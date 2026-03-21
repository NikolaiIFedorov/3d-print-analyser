#include "SceneRenderer.hpp"
#include "utils/log.hpp"

void SceneRenderer::UpdateFromRenderBuffer(const Scene &scene)
{
    GLint viewPort[4];
    glGetIntegerv(GL_VIEWPORT, viewPort);

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    wireframe.Generate(renderBuffer, vertices, indices);
    renderer.UploadLineMesh(vertices, indices);

    vertices.clear();
    indices.clear();

    patch.Generate(renderBuffer, vertices, indices, viewPort);
    renderer.UploadTriangleMesh(vertices, indices);
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

    renderer.DrawTriangles();

    renderer.DrawLines();

    renderer.EndFrame();
}

void SceneRenderer::Shutdown()
{
    renderer.Shutdown();
}

void SceneRenderer::AddForm(FormPtr form)
{
    renderBuffer.AddForm(form);
}
