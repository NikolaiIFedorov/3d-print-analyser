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
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class SceneRenderer
{
public:
    struct GeometryInvalidationScope
    {
        bool fullScene = false;
        std::unordered_set<const Solid *> solids;
        std::unordered_set<const Face *> faces;
        std::unordered_set<const Edge *> edges;
    };

    Wireframe wireframe;
    Patch patch;

    SceneRenderer() {};

    SceneRenderer(SDL_Window *window) : renderer(window) {};

    void UpdateScene(Scene *scene, const AnalysisResults *results = nullptr);
    void RebuildAll(Scene *scene, const AnalysisResults *results = nullptr);
    /// Time-sliced variant of `RebuildAll` for large scenes. Returns true when finished.
    bool RebuildAllIncremental(Scene *scene, const AnalysisResults *results, double budgetMs,
                               uint64_t analysisIdentity = 0);
    bool FullRebuildInProgress() const { return fullRebuildInProgress; }
    void RebuildSolids(Scene *scene, const std::unordered_set<const Solid *> &dirtySolids,
                       const AnalysisResults *results = nullptr);
    void RecolorOnly(Scene *scene, const AnalysisResults *results = nullptr);
    void RebuildScope(Scene *scene, const GeometryInvalidationScope &scope, const AnalysisResults *results = nullptr);

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
    uint64_t FullRebuildCount() const { return fullRebuildCount; }
    uint64_t PartialRebuildCount() const { return partialRebuildCount; }
    uint64_t RecolorOnlyCount() const { return recolorOnlyCount; }

    void Shutdown();

private:
    struct MeshChunk
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        size_t vertexOffset = 0;
        size_t indexOffset = 0;
    };

    struct SolidChunk
    {
        MeshChunk wireframe;
        MeshChunk patch;
        std::vector<PickTriangle> pickTriangles;
    };

    void RebuildLoose(Scene *scene, const AnalysisResults *results);
    void BuildSolidChunk(const Solid *solid, const AnalysisResults *results, SolidChunk &out) const;
    void RepackOffsets();
    bool UploadChunkSubData(const Solid *solid, const SolidChunk &chunk);
    void UploadAllPacked();
    void RebuildPickSegments(Scene *scene);
    void RebuildPickTriangles();
    void AbortIncrementalFullRebuild();

    OpenGLRenderer renderer;
    std::vector<const Solid *> solidOrder;
    std::unordered_map<const Solid *, SolidChunk> solidChunks;
    MeshChunk looseWireframe;
    MeshChunk loosePatch;
    bool packedUploaded = false;
    uint64_t fullRebuildCount = 0;
    uint64_t partialRebuildCount = 0;
    uint64_t recolorOnlyCount = 0;
    std::vector<PickTriangle> pickTriangles;
    std::vector<PickSegment> pickSegments;

    enum class FullRebuildPhase
    {
        Idle,
        BuildingSolids,
        RebuildingLoose,
        Repacking,
        Uploading,
        PickRebuild,
        Done
    };

    bool fullRebuildInProgress = false;
    FullRebuildPhase fullRebuildPhase = FullRebuildPhase::Idle;
    size_t fullRebuildSolidIndex = 0;
    Scene *fullRebuildScene = nullptr;
    /// Which analysis snapshot this rebuild incorporates (opaque id from `Display`; 0 = none).
    uint64_t fullRebuildAnalysisIdentity = 0;
    const AnalysisResults *fullRebuildResults = nullptr;
}; 