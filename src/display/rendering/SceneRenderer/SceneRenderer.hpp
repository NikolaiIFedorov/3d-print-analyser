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

#include <utility>

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

    /// Optional Structure-tool preview edge loops, rebuilt each upload.
    void SetStructurePreviewSegments(std::vector<std::pair<glm::vec3, glm::vec3>> segments);

    /// Optional Analysis-tool debug overlay: the raw sliced cross-section loops, rebuilt each upload.
    void SetAnalysisDebugSegments(std::vector<std::pair<glm::vec3, glm::vec3>> segments);

    /// Filled layer-diff overlay: earcuts the per-layer RingDifference result and uploads as translucent triangles.
    void SetAnalysisDebugLayerDiff(const std::vector<std::vector<glm::dvec3>> &rings, float z);

    /// Uploads all overhang clipBoundary polygons from analysis results as flat horizontal overlays.
    /// Pass nullptr to clear.
    void SetOverhangOverlays(const AnalysisResults *results);

    /// Extracts SHARP_CORNER edge lines from analysis results and uploads for line rendering.
    /// Pass nullptr to clear.
    void SetSharpCornerEdges(const AnalysisResults *results);

    /// Per frame: translucent solid shell while Structure view is active (depth prepass + alpha in OpenGLRenderer).
    void SetStructureViewTranslucentSolid(bool enable, float alpha01);

    void SetCamera(Camera &camera);

    void RenderPatches();
    void RenderPickHighlight();
    void RenderPickHighlightCalibInvalid();
    void RenderPickHighlightReject();
    void RenderPickHighlightXray();
    void RenderPickHighlightLines(float lineWidthPx);
    void RenderPickHighlightLinesXray(float lineWidthPx);
    void RenderCalibHoverSpanLine(float lineWidthPx, bool xrayOverlay = false);
    void RenderOpenBoundaryBlameFace(bool xrayOverlay = false);
    void RenderOpenBoundaryBlameLine(float lineWidthPx, bool xrayOverlay = false);
    /// Dedicated mesh from `structurePreviewSegments`; foreground + occluded passes.
    void RenderStructurePreviewLines(float lineWidthPx);
    /// Dedicated mesh from `analysisDebugSegments`; foreground + occluded passes.
    void RenderAnalysisDebugLines(float lineWidthPx);
    /// Filled layer-diff overlay triangles (translucent, depth-read-only).
    void RenderAnalysisDebugTriangles();
    /// Flat horizontal overhang region overlays (translucent, depth-read-only).
    void RenderOverhangOverlays();
    /// Colored edge lines for SHARP_CORNER sharp corners.
    void RenderSharpCornerEdges(float lineWidthPx);
    void RenderWireframe();
    /// Second pass: first `packedSolidWireframeIndexCount` line indices (solid wire only), depth-behind shell.
    void RenderSolidWireframeOccludedOverlay();

    void UploadPickHighlightMesh(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices,
                                 uint32_t xrayIndexCount = 0);
    void UploadPickHighlightRejectMesh(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);
    void UploadPickHighlightCalibInvalidMesh(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);
    void UploadPickHighlightLineMesh(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices,
                                     uint32_t xrayIndexCount = 0);
    void UploadCalibHoverSpanLineMesh(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);
    void UploadOpenBoundaryBlameFaceMesh(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);
    void UploadOpenBoundaryBlameLineMesh(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);

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

    /// Monotonic step counter for incremental full rebuild (solids mesh + trailing stages). Idle → zeros.
    uint64_t IncrementalRebuildStepsDone() const { return incrementalRebuildStepsDone; }
    uint64_t IncrementalRebuildStepsTotal() const { return incrementalRebuildStepsTotal; }

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
    void CommitStructurePreviewLinesToGpu();
    void CommitAnalysisDebugLinesToGpu();
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

    std::vector<std::pair<glm::vec3, glm::vec3>> structurePreviewSegments;
    std::vector<std::pair<glm::vec3, glm::vec3>> analysisDebugSegments;
    std::vector<std::pair<glm::vec3, glm::vec3>> sharpCornerEdgeSegments;

    uint32_t packedSolidPatchIndexCount = 0;
    /// Line indices belonging to solid wireframe chunks only (prefix of packed line IBO); set in `RepackOffsets`.
    uint32_t packedSolidWireframeIndexCount = 0;
    bool structureViewTranslucentSolidEnabled = false;
    float structureViewTranslucentSolidAlpha = 0.32f;

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

    uint64_t incrementalRebuildStepsTotal = 0;
    uint64_t incrementalRebuildStepsDone = 0;
}; 