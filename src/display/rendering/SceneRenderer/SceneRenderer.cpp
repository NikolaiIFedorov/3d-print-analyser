#include "SceneRenderer.hpp"
#include "ProjectionDepthMode.hpp"
#include "rendering/CalibPickSegments.hpp"
#include "utils/log.hpp"
#include <chrono>

namespace
{
using Clock = std::chrono::steady_clock;

inline double MsSince(const Clock::time_point &start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

inline void LogSlowStage(const char *stage, double ms)
{
    // Keep output low-noise; only log slow render-rebuild stages.
    if (ms >= 4.0)
        LOG_SESSION("Render stage", stage, "ms", ms);
}
} // namespace

void SceneRenderer::AbortIncrementalFullRebuild()
{
    fullRebuildInProgress = false;
    fullRebuildPhase = FullRebuildPhase::Idle;
    fullRebuildSolidIndex = 0;
    fullRebuildScene = nullptr;
    fullRebuildAnalysisIdentity = 0;
    fullRebuildResults = nullptr;
}

void SceneRenderer::UpdateScene(Scene *scene, const AnalysisResults *results)
{
    RebuildAll(scene, results);
}

void SceneRenderer::RebuildAll(Scene *scene, const AnalysisResults *results)
{
    AbortIncrementalFullRebuild();
    const Clock::time_point tAll = Clock::now();
    fullRebuildCount++;
    solidOrder.clear();
    solidChunks.clear();

    const Clock::time_point tBuildChunks = Clock::now();
    for (const Solid &solid : scene->solids)
    {
        const Solid *key = &solid;
        solidOrder.push_back(key);
        SolidChunk chunk;
        BuildSolidChunk(key, results, chunk);
        solidChunks.emplace(key, std::move(chunk));
    }
    LogSlowStage("build_chunks", MsSince(tBuildChunks));

    const Clock::time_point tLoose = Clock::now();
    RebuildLoose(scene, results);
    LogSlowStage("rebuild_loose", MsSince(tLoose));
    const Clock::time_point tRepack = Clock::now();
    RepackOffsets();
    LogSlowStage("repack_offsets", MsSince(tRepack));
    const Clock::time_point tUpload = Clock::now();
    UploadAllPacked();
    LogSlowStage("upload_all_packed", MsSince(tUpload));
    const Clock::time_point tPickTris = Clock::now();
    RebuildPickTriangles();
    LogSlowStage("rebuild_pick_tris", MsSince(tPickTris));
    const Clock::time_point tPickSegs = Clock::now();
    RebuildPickSegments(scene);
    LogSlowStage("rebuild_pick_segs", MsSince(tPickSegs));
    LogSlowStage("rebuild_all_total", MsSince(tAll));
}

bool SceneRenderer::RebuildAllIncremental(Scene *scene, const AnalysisResults *results, double budgetMs,
                                          uint64_t analysisIdentity)
{
    if (scene == nullptr)
    {
        AbortIncrementalFullRebuild();
        return true;
    }

    const Clock::time_point tBudgetStart = Clock::now();
    auto budgetLeft = [&]() -> double
    {
        return budgetMs - MsSince(tBudgetStart);
    };

    if (fullRebuildPhase == FullRebuildPhase::Idle)
    {
        // Start a new incremental rebuild session.
        fullRebuildCount++;
        solidOrder.clear();
        solidChunks.clear();

        for (const Solid &solid : scene->solids)
            solidOrder.push_back(&solid);

        fullRebuildScene = scene;
        fullRebuildResults = results;
        fullRebuildAnalysisIdentity = analysisIdentity;
        fullRebuildSolidIndex = 0;
        fullRebuildPhase = FullRebuildPhase::BuildingSolids;
        fullRebuildInProgress = true;
    }
    else if (fullRebuildScene != scene || fullRebuildAnalysisIdentity != analysisIdentity)
    {
        // Scene or analysis snapshot changed mid-flight; restart safely.
        AbortIncrementalFullRebuild();
        return RebuildAllIncremental(scene, results, budgetLeft(), analysisIdentity);
    }

    while (fullRebuildPhase != FullRebuildPhase::Done)
    {
        if (budgetLeft() <= 0.0)
            return false;

        switch (fullRebuildPhase)
        {
        case FullRebuildPhase::BuildingSolids:
        {
            const Clock::time_point tBuild = Clock::now();
            while (fullRebuildSolidIndex < solidOrder.size())
            {
                if (budgetLeft() <= 0.0)
                {
                    LogSlowStage("build_chunks_partial", MsSince(tBuild));
                    return false;
                }

                const Solid *key = solidOrder[fullRebuildSolidIndex];
                SolidChunk chunk;
                BuildSolidChunk(key, fullRebuildResults, chunk);
                solidChunks.emplace(key, std::move(chunk));
                ++fullRebuildSolidIndex;
            }
            LogSlowStage("build_chunks", MsSince(tBuild));
            fullRebuildPhase = FullRebuildPhase::RebuildingLoose;
            break;
        }
        case FullRebuildPhase::RebuildingLoose:
        {
            const Clock::time_point tLoose = Clock::now();
            RebuildLoose(fullRebuildScene, fullRebuildResults);
            LogSlowStage("rebuild_loose", MsSince(tLoose));
            fullRebuildPhase = FullRebuildPhase::Repacking;
            break;
        }
        case FullRebuildPhase::Repacking:
        {
            const Clock::time_point tRepack = Clock::now();
            RepackOffsets();
            LogSlowStage("repack_offsets", MsSince(tRepack));
            fullRebuildPhase = FullRebuildPhase::Uploading;
            break;
        }
        case FullRebuildPhase::Uploading:
        {
            const Clock::time_point tUpload = Clock::now();
            UploadAllPacked();
            LogSlowStage("upload_all_packed", MsSince(tUpload));
            fullRebuildPhase = FullRebuildPhase::PickRebuild;
            break;
        }
        case FullRebuildPhase::PickRebuild:
        {
            const Clock::time_point tPickTris = Clock::now();
            RebuildPickTriangles();
            LogSlowStage("rebuild_pick_tris", MsSince(tPickTris));
            const Clock::time_point tPickSegs = Clock::now();
            RebuildPickSegments(fullRebuildScene);
            LogSlowStage("rebuild_pick_segs", MsSince(tPickSegs));
            fullRebuildPhase = FullRebuildPhase::Done;
            break;
        }
        case FullRebuildPhase::Done:
        default:
            break;
        }
    }

    LogSlowStage("rebuild_all_total_incremental_session", MsSince(tBudgetStart));

    AbortIncrementalFullRebuild();
    return true;
}

void SceneRenderer::RebuildSolids(Scene *scene, const std::unordered_set<const Solid *> &dirtySolids,
                                  const AnalysisResults *results)
{
    if (dirtySolids.empty())
        return;

    AbortIncrementalFullRebuild();
    partialRebuildCount++;
    bool missingChunk = false;
    for (const Solid *solid : dirtySolids)
    {
        if (solidChunks.find(solid) == solidChunks.end())
        {
            missingChunk = true;
            break;
        }
    }

    if (missingChunk)
    {
        RebuildAll(scene, results);
        return;
    }

    bool canSubUpdate = packedUploaded;
    for (const Solid *solid : dirtySolids)
    {
        auto it = solidChunks.find(solid);
        if (it == solidChunks.end())
            continue;

        SolidChunk updated;
        BuildSolidChunk(solid, results, updated);
        const bool sizeChanged =
            updated.wireframe.vertices.size() != it->second.wireframe.vertices.size() ||
            updated.wireframe.indices.size() != it->second.wireframe.indices.size() ||
            updated.patch.vertices.size() != it->second.patch.vertices.size() ||
            updated.patch.indices.size() != it->second.patch.indices.size();
        if (sizeChanged)
            canSubUpdate = false;
        it->second = std::move(updated);
    }

    if (!canSubUpdate)
    {
        RepackOffsets();
        UploadAllPacked();
    }
    else
    {
        for (const Solid *solid : dirtySolids)
        {
            auto it = solidChunks.find(solid);
            if (it == solidChunks.end())
                continue;
            if (!UploadChunkSubData(solid, it->second))
            {
                RepackOffsets();
                UploadAllPacked();
                break;
            }
        }
    }

    RebuildPickTriangles();
    RebuildPickSegments(scene);
}

void SceneRenderer::RebuildScope(Scene *scene, const GeometryInvalidationScope &scope, const AnalysisResults *results)
{
    if (scope.fullScene)
    {
        RebuildAll(scene, results);
        return;
    }

    if (!scope.solids.empty())
    {
        RebuildSolids(scene, scope.solids, results);
        return;
    }

    // Metadata-only face/edge invalidation currently escalates to full rebuild for safety.
    if (!scope.faces.empty() || !scope.edges.empty())
    {
        RebuildAll(scene, results);
        return;
    }
}

void SceneRenderer::RecolorOnly(Scene *scene, const AnalysisResults *results)
{
    AbortIncrementalFullRebuild();
    recolorOnlyCount++;

    // Rebuild chunks to refresh per-vertex colors while preserving topology where possible.
    for (const Solid *solid : solidOrder)
    {
        auto it = solidChunks.find(solid);
        if (it == solidChunks.end())
            continue;

        SolidChunk recolored;
        BuildSolidChunk(solid, results, recolored);
        const bool sameLayout =
            recolored.wireframe.vertices.size() == it->second.wireframe.vertices.size() &&
            recolored.wireframe.indices.size() == it->second.wireframe.indices.size() &&
            recolored.patch.vertices.size() == it->second.patch.vertices.size() &&
            recolored.patch.indices.size() == it->second.patch.indices.size();
        if (!sameLayout)
        {
            RebuildAll(scene, results);
            return;
        }
        recolored.wireframe.vertexOffset = it->second.wireframe.vertexOffset;
        recolored.wireframe.indexOffset = it->second.wireframe.indexOffset;
        recolored.patch.vertexOffset = it->second.patch.vertexOffset;
        recolored.patch.indexOffset = it->second.patch.indexOffset;
        it->second = std::move(recolored);
        if (!UploadChunkSubData(solid, it->second))
        {
            RebuildAll(scene, results);
            return;
        }
    }

    const MeshChunk oldLooseWire = looseWireframe;
    const MeshChunk oldLoosePatch = loosePatch;
    RebuildLoose(scene, results);
    const bool looseLayoutSame =
        oldLooseWire.vertices.size() == looseWireframe.vertices.size() &&
        oldLooseWire.indices.size() == looseWireframe.indices.size() &&
        oldLoosePatch.vertices.size() == loosePatch.vertices.size() &&
        oldLoosePatch.indices.size() == loosePatch.indices.size();
    if (!looseLayoutSame)
    {
        RepackOffsets();
        UploadAllPacked();
    }
    else
    {
        looseWireframe.vertexOffset = oldLooseWire.vertexOffset;
        looseWireframe.indexOffset = oldLooseWire.indexOffset;
        loosePatch.vertexOffset = oldLoosePatch.vertexOffset;
        loosePatch.indexOffset = oldLoosePatch.indexOffset;

        std::vector<uint32_t> looseLineIndices = looseWireframe.indices;
        for (uint32_t &idx : looseLineIndices)
            idx += static_cast<uint32_t>(looseWireframe.vertexOffset);
        std::vector<uint32_t> looseTriIndices = loosePatch.indices;
        for (uint32_t &idx : looseTriIndices)
            idx += static_cast<uint32_t>(loosePatch.vertexOffset);

        if (!renderer.UpdateLineMeshSubData(looseWireframe.vertices, looseWireframe.vertexOffset,
                                            looseLineIndices, looseWireframe.indexOffset) ||
            !renderer.UpdateTriangleMeshSubData(loosePatch.vertices, loosePatch.vertexOffset,
                                                looseTriIndices, loosePatch.indexOffset))
        {
            RebuildAll(scene, results);
            return;
        }
    }

    RebuildPickTriangles();
    RebuildPickSegments(scene);
}

void SceneRenderer::RebuildLoose(Scene *scene, const AnalysisResults *results)
{
    looseWireframe.vertices.clear();
    looseWireframe.indices.clear();
    wireframe.GenerateLoose(scene, looseWireframe.vertices, looseWireframe.indices);

    GLint viewPort[4];
    glGetIntegerv(GL_VIEWPORT, viewPort);
    loosePatch.vertices.clear();
    loosePatch.indices.clear();
    patch.GenerateLoose(scene, loosePatch.vertices, loosePatch.indices, viewPort, results, nullptr);
}

void SceneRenderer::BuildSolidChunk(const Solid *solid, const AnalysisResults *results, SolidChunk &out) const
{
    out.wireframe.vertices.clear();
    out.wireframe.indices.clear();
    out.patch.vertices.clear();
    out.patch.indices.clear();
    out.pickTriangles.clear();

    GLint viewPort[4];
    glGetIntegerv(GL_VIEWPORT, viewPort);
    wireframe.GenerateSolid(solid, out.wireframe.vertices, out.wireframe.indices, results);
    patch.GenerateSolid(solid, out.patch.vertices, out.patch.indices, viewPort, results, &out.pickTriangles);
}

void SceneRenderer::RepackOffsets()
{
    size_t lineV = 0;
    size_t lineI = 0;
    size_t triV = 0;
    size_t triI = 0;
    for (const Solid *solid : solidOrder)
    {
        auto it = solidChunks.find(solid);
        if (it == solidChunks.end())
            continue;
        it->second.wireframe.vertexOffset = lineV;
        it->second.wireframe.indexOffset = lineI;
        lineV += it->second.wireframe.vertices.size();
        lineI += it->second.wireframe.indices.size();

        it->second.patch.vertexOffset = triV;
        it->second.patch.indexOffset = triI;
        triV += it->second.patch.vertices.size();
        triI += it->second.patch.indices.size();
    }

    looseWireframe.vertexOffset = lineV;
    looseWireframe.indexOffset = lineI;
    loosePatch.vertexOffset = triV;
    loosePatch.indexOffset = triI;
}

bool SceneRenderer::UploadChunkSubData(const Solid *solid, const SolidChunk &chunk)
{
    (void)solid;
    std::vector<uint32_t> lineIndices = chunk.wireframe.indices;
    for (uint32_t &idx : lineIndices)
        idx += static_cast<uint32_t>(chunk.wireframe.vertexOffset);
    std::vector<uint32_t> triIndices = chunk.patch.indices;
    for (uint32_t &idx : triIndices)
        idx += static_cast<uint32_t>(chunk.patch.vertexOffset);

    const bool lineOk = renderer.UpdateLineMeshSubData(chunk.wireframe.vertices, chunk.wireframe.vertexOffset,
                                                       lineIndices, chunk.wireframe.indexOffset);
    const bool triOk = renderer.UpdateTriangleMeshSubData(chunk.patch.vertices, chunk.patch.vertexOffset,
                                                          triIndices, chunk.patch.indexOffset);
    return lineOk && triOk;
}

void SceneRenderer::UploadAllPacked()
{
    std::vector<Vertex> lineVertices;
    std::vector<uint32_t> lineIndices;
    std::vector<Vertex> triVertices;
    std::vector<uint32_t> triIndices;

    size_t reserveLineV = looseWireframe.vertices.size();
    size_t reserveLineI = looseWireframe.indices.size();
    size_t reserveTriV = loosePatch.vertices.size();
    size_t reserveTriI = loosePatch.indices.size();
    for (const Solid *solid : solidOrder)
    {
        auto it = solidChunks.find(solid);
        if (it == solidChunks.end())
            continue;
        reserveLineV += it->second.wireframe.vertices.size();
        reserveLineI += it->second.wireframe.indices.size();
        reserveTriV += it->second.patch.vertices.size();
        reserveTriI += it->second.patch.indices.size();
    }

    lineVertices.reserve(reserveLineV);
    lineIndices.reserve(reserveLineI);
    triVertices.reserve(reserveTriV);
    triIndices.reserve(reserveTriI);

    for (const Solid *solid : solidOrder)
    {
        auto it = solidChunks.find(solid);
        if (it == solidChunks.end())
            continue;

        const uint32_t lineBase = static_cast<uint32_t>(lineVertices.size());
        lineVertices.insert(lineVertices.end(), it->second.wireframe.vertices.begin(), it->second.wireframe.vertices.end());
        for (uint32_t idx : it->second.wireframe.indices)
            lineIndices.push_back(lineBase + idx);

        const uint32_t triBase = static_cast<uint32_t>(triVertices.size());
        triVertices.insert(triVertices.end(), it->second.patch.vertices.begin(), it->second.patch.vertices.end());
        for (uint32_t idx : it->second.patch.indices)
            triIndices.push_back(triBase + idx);
    }

    const uint32_t looseLineBase = static_cast<uint32_t>(lineVertices.size());
    lineVertices.insert(lineVertices.end(), looseWireframe.vertices.begin(), looseWireframe.vertices.end());
    for (uint32_t idx : looseWireframe.indices)
        lineIndices.push_back(looseLineBase + idx);

    const uint32_t looseTriBase = static_cast<uint32_t>(triVertices.size());
    triVertices.insert(triVertices.end(), loosePatch.vertices.begin(), loosePatch.vertices.end());
    for (uint32_t idx : loosePatch.indices)
        triIndices.push_back(looseTriBase + idx);

    renderer.UploadLineMesh(lineVertices, lineIndices);
    renderer.UploadTriangleMesh(triVertices, triIndices);
    packedUploaded = true;
}

void SceneRenderer::RebuildPickSegments(Scene *scene)
{
    CalibPickSegments::Build(scene, pickSegments);
}

void SceneRenderer::RebuildPickTriangles()
{
    pickTriangles.clear();
    for (const Solid *solid : solidOrder)
    {
        auto it = solidChunks.find(solid);
        if (it == solidChunks.end())
            continue;
        pickTriangles.insert(pickTriangles.end(), it->second.pickTriangles.begin(), it->second.pickTriangles.end());
    }
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
    AbortIncrementalFullRebuild();
    renderer.Shutdown();
    solidOrder.clear();
    solidChunks.clear();
    looseWireframe = {};
    loosePatch = {};
    packedUploaded = false;
}