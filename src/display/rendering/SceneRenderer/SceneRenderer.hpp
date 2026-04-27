#pragma once

#include "glad/glad.h"
#include "SDL3/SDL.h"

#include "rendering/OpenGL/OpenGLRenderer.hpp"
#include "rendering/Camera/camera.hpp"
#include "rendering/color.hpp"
#include "rendering/ScenePick.hpp"

#include "Wireframe/Wireframe.hpp"
#include "Patch/patch.hpp"

#include "scene/scene.hpp"

#include "mapbox/earcut.hpp"

class SceneRenderer
{
public:
    Wireframe wireframe;
    Patch patch;

    SceneRenderer() {};

    SceneRenderer(SDL_Window *window) : renderer(window) {};

    void UpdateScene(Scene *scene, const AnalysisResults *results = nullptr);

    void SetCamera(Camera &camera);

    void RenderPatches();
    void RenderPickHighlight();
    void RenderPickHighlightLines(float lineWidthPx);
    void RenderWireframe();

    void UploadPickHighlightMesh(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);
    void UploadPickHighlightLineMesh(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);

    const std::vector<PickTriangle> &GetPickTriangles() const { return pickTriangles; }
    const std::vector<PickSegment> &GetPickSegments() const { return pickSegments; }

    /// Last uploaded main scene mesh (patches + wireframe), for HUD / diagnostics.
    uint32_t UploadedTriangleIndexCount() const { return renderer.GetTriangleIndexCount(); }
    uint32_t UploadedLineIndexCount() const { return renderer.GetLineIndexCount(); }
    uint32_t UploadedTriangleVertexCount() const { return renderer.GetTriangleVertexCount(); }
    uint32_t UploadedLineVertexCount() const { return renderer.GetLineVertexCount(); }

    void Shutdown();

private:
    OpenGLRenderer renderer;
    std::vector<PickTriangle> pickTriangles;
    std::vector<PickSegment> pickSegments;
};