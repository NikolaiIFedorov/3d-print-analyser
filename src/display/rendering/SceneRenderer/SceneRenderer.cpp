#include "SceneRenderer.hpp"
#include "ProjectionDepthMode.hpp"
#include "rendering/CalibPickSegments.hpp"
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

    pickTriangles.clear();
    patch.Generate(scene, vertices, indices, viewPort, results, &pickTriangles);
    renderer.UploadTriangleMesh(vertices, indices);

    CalibPickSegments::Build(scene, pickSegments);
}

void SceneRenderer::UploadPickHighlightMesh(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices)
{
    renderer.UploadPickHighlightMesh(vertices, indices);
}

void SceneRenderer::UploadPickHighlightLineMesh(const std::vector<Vertex> &vertices,
                                                const std::vector<uint32_t> &indices)
{
    renderer.UploadPickHighlightLineMesh(vertices, indices);
}

void SceneRenderer::RenderPickHighlight()
{
    renderer.DrawPickHighlight();
}

void SceneRenderer::RenderPickHighlightLines(float lineWidthPx)
{
    renderer.DrawPickHighlightLines(lineWidthPx);
}

void SceneRenderer::SetCamera(Camera &camera)
{

    renderer.SetViewMatrix(camera.GetViewMatrix());
    renderer.SetProjectionMatrix(
        ProjectionDepthMode::EffectiveProjection(camera.GetProjectionMatrix()));
    renderer.SetModelMatrix(glm::mat4(1.0f));
    renderer.SetViewPos(camera.GetPosition());
    // Principal-axis views tighten depth precision; nudge wireframe slightly more so back edges
    // stay behind filled surfaces.
    renderer.SetWireframeDepthNudgeScale(camera.IsPrincipalAxisView() ? 2.25f : 1.0f);
}

void SceneRenderer::RenderPatches()
{
    renderer.DrawTriangles();
}

void SceneRenderer::RenderWireframe()
{
    renderer.DrawLines();
}

void SceneRenderer::Shutdown()
{
    renderer.Shutdown();
}