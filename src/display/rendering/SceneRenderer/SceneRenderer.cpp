#include "SceneRenderer.hpp"
#include "utils/log.hpp"

void SceneRenderer::UpdateScene(Scene *scene, const AnalysisResults *results)
{
    GLint viewPort[4];
    glGetIntegerv(GL_VIEWPORT, viewPort);

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    wireframe.Generate(scene, vertices, indices, results);
    renderer.UploadLineMesh(vertices, indices);

    vertices.clear();
    indices.clear();

    patch.Generate(scene, vertices, indices, viewPort, results);
    renderer.UploadTriangleMesh(vertices, indices);
}

void SceneRenderer::SetCamera(Camera &camera)
{

    renderer.SetViewMatrix(camera.GetViewMatrix());
    renderer.SetProjectionMatrix(camera.GetProjectionMatrix());
    renderer.SetModelMatrix(glm::mat4(1.0f));
    renderer.SetViewPos(camera.GetPosition());
}

void SceneRenderer::Render()
{
    renderer.DrawTriangles();
    renderer.DrawLines();
}

void SceneRenderer::Shutdown()
{
    renderer.Shutdown();
}