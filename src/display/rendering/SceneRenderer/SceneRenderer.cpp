#include "SceneRenderer.hpp"
#include "ProjectionDepthMode.hpp"
#include "rendering/CalibPickSegments.hpp"
#include "Geometry/Geometry.hpp"
#include "rendering/color.hpp"
#include "utils/log.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace
{
using Clock = std::chrono::steady_clock;

// -- Z-clipped face overlay helpers --

static void OverlayPlaneCoordinateSystem(const glm::dvec3 &normal,
                                         glm::dvec3 &uAxis, glm::dvec3 &vAxis)
{
    glm::dvec3 arb = (std::abs(normal.x) < 0.9) ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
    uAxis = glm::normalize(glm::cross(normal, arb));
    vAxis = glm::normalize(glm::cross(normal, uAxis));
}

static glm::dvec2 OverlayProjectPoint(const glm::dvec3 &pt, const glm::dvec3 &origin,
                                       const glm::dvec3 &u, const glm::dvec3 &v)
{
    glm::dvec3 rel = pt - origin;
    return {glm::dot(rel, u), glm::dot(rel, v)};
}

static std::vector<glm::dvec3> OverlayTessellateCurve(const Curve *curve,
                                                        const glm::dvec3 &start,
                                                        const glm::dvec3 &end,
                                                        int segments)
{
    std::vector<glm::dvec3> pts;
    if (!curve)
    {
        pts.push_back(start);
        pts.push_back(end);
        return pts;
    }
    for (int i = 0; i <= segments; i++)
        pts.push_back(curve->Evaluate((double)i / segments, start, end));
    return pts;
}

static std::vector<glm::dvec3> OverlayClipLoopByZ(const std::vector<glm::dvec3> &loop,
                                                    double zPlane, bool keepAbove)
{
    std::vector<glm::dvec3> out;
    if (loop.empty()) return out;
    auto inside = [&](const glm::dvec3 &p) {
        return keepAbove ? p.z >= zPlane - 1e-10 : p.z <= zPlane + 1e-10;
    };
    for (size_t i = 0; i < loop.size(); i++)
    {
        const glm::dvec3 &curr = loop[i];
        const glm::dvec3 &next = loop[(i + 1) % loop.size()];
        bool cIn = inside(curr), nIn = inside(next);
        if (cIn) out.push_back(curr);
        if (cIn != nIn)
        {
            double dz = next.z - curr.z;
            if (std::abs(dz) > 1e-14)
            {
                double t = (zPlane - curr.z) / dz;
                out.push_back(curr + t * (next - curr));
            }
        }
    }
    return out;
}

static void OverlayTriangulateFaceZClipped(const Face *face, const ZBounds &zb,
                                            const glm::vec4 &color,
                                            std::vector<AnalysisTriVertex> &vertices,
                                            std::vector<uint32_t> &indices)
{
    if (!face || !face->HasGeometry()) return;

    glm::dvec3 uAxis, vAxis;
    OverlayPlaneCoordinateSystem(face->GetSurface().GetNormal(), uAxis, vAxis);

    using Point2D = std::array<double, 2>;
    std::vector<std::vector<Point2D>> polygon;
    std::vector<std::vector<glm::dvec3>> allPositions;
    glm::dvec3 origin(0, 0, 0);
    bool originSet = false;

    for (const auto &edgeLoop : face->loops)
    {
        std::vector<glm::dvec3> loopPos;
        for (const auto &oe : edgeLoop)
        {
            const Point *p0 = oe.GetStart();
            if (!originSet) { origin = p0->position; originSet = true; }
            if (!oe.edge->curve)
            {
                loopPos.push_back(p0->position);
            }
            else
            {
                auto tess = OverlayTessellateCurve(oe.edge->curve,
                                                    oe.GetStartPosition(), oe.GetEndPosition(), 16);
                for (size_t j = 0; j + 1 < tess.size(); j++)
                    loopPos.push_back(tess[j]);
            }
        }
        loopPos = OverlayClipLoopByZ(loopPos, zb.zMin, true);
        loopPos = OverlayClipLoopByZ(loopPos, zb.zMax, false);
        if (loopPos.size() < 3) continue;

        std::vector<Point2D> loop2D;
        for (const auto &pos : loopPos)
        {
            auto p2 = OverlayProjectPoint(pos, origin, uAxis, vAxis);
            loop2D.push_back({p2.x, p2.y});
        }
        allPositions.push_back(loopPos);
        polygon.push_back(loop2D);
    }

    if (polygon.empty()) return;

    // Consistent winding: outer CCW, inner CW
    auto signedArea = [](const std::vector<Point2D> &l) {
        double a = 0;
        for (size_t j = 0; j < l.size(); j++) {
            size_t k = (j + 1) % l.size();
            a += l[j][0] * l[k][1] - l[k][0] * l[j][1];
        }
        return a;
    };
    if (signedArea(polygon[0]) < 0) {
        std::reverse(polygon[0].begin(), polygon[0].end());
        std::reverse(allPositions[0].begin(), allPositions[0].end());
    }
    for (size_t li = 1; li < polygon.size(); li++) {
        if (signedArea(polygon[li]) > 0) {
            std::reverse(polygon[li].begin(), polygon[li].end());
            std::reverse(allPositions[li].begin(), allPositions[li].end());
        }
    }

    auto triIdx = mapbox::earcut<uint32_t>(polygon);
    if (triIdx.empty()) return;

    uint32_t base = static_cast<uint32_t>(vertices.size());
    for (const auto &lp : allPositions)
        for (const auto &pos : lp)
            vertices.push_back({glm::vec3(pos), color});
    for (uint32_t idx : triIdx)
        indices.push_back(base + idx);
}

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

static void AppendPreviewLineSegments(const std::vector<std::pair<glm::vec3, glm::vec3>> &segments,
                                      const glm::vec3 &lineRgb, std::vector<Vertex> &vertices,
                                      std::vector<uint32_t> &indices)
{
    if (segments.empty())
        return;
    const glm::vec3 nrm(0.0f);
    vertices.reserve(vertices.size() + segments.size() * 2);
    indices.reserve(indices.size() + segments.size() * 2);
    for (const auto &seg : segments)
    {
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        Vertex v0{};
        v0.position = seg.first;
        v0.color = lineRgb;
        v0.normal = nrm;
        Vertex v1{};
        v1.position = seg.second;
        v1.color = lineRgb;
        v1.normal = nrm;
        vertices.push_back(v0);
        vertices.push_back(v1);
        indices.push_back(base);
        indices.push_back(base + 1u);
    }
}
} // namespace

void SceneRenderer::SetStructurePreviewSegments(std::vector<std::pair<glm::vec3, glm::vec3>> segments)
{
    structurePreviewSegments = std::move(segments);
    CommitStructurePreviewLinesToGpu();
}

void SceneRenderer::SetAnalysisDebugSegments(std::vector<std::pair<glm::vec3, glm::vec3>> segments)
{
    analysisDebugSegments = std::move(segments);
    CommitAnalysisDebugLinesToGpu();
}

void SceneRenderer::SetStructureViewTranslucentSolid(bool enable, float alpha01)
{
    structureViewTranslucentSolidEnabled = enable;
    structureViewTranslucentSolidAlpha = std::clamp(alpha01, 0.06f, 0.92f);
}

void SceneRenderer::AbortIncrementalFullRebuild()
{
    fullRebuildInProgress = false;
    fullRebuildPhase = FullRebuildPhase::Idle;
    fullRebuildSolidIndex = 0;
    fullRebuildScene = nullptr;
    fullRebuildAnalysisIdentity = 0;
    fullRebuildResults = nullptr;
    incrementalRebuildStepsDone = 0;
    incrementalRebuildStepsTotal = 0;
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
        incrementalRebuildStepsDone = 0;
        incrementalRebuildStepsTotal = solidOrder.size() + 4u;
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
                ++incrementalRebuildStepsDone;
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
            ++incrementalRebuildStepsDone;
            fullRebuildPhase = FullRebuildPhase::Repacking;
            break;
        }
        case FullRebuildPhase::Repacking:
        {
            const Clock::time_point tRepack = Clock::now();
            RepackOffsets();
            LogSlowStage("repack_offsets", MsSince(tRepack));
            ++incrementalRebuildStepsDone;
            fullRebuildPhase = FullRebuildPhase::Uploading;
            break;
        }
        case FullRebuildPhase::Uploading:
        {
            const Clock::time_point tUpload = Clock::now();
            UploadAllPacked();
            LogSlowStage("upload_all_packed", MsSince(tUpload));
            ++incrementalRebuildStepsDone;
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
            ++incrementalRebuildStepsDone;
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
        CommitStructurePreviewLinesToGpu();
        CommitAnalysisDebugLinesToGpu();
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
    CommitStructurePreviewLinesToGpu();
    CommitAnalysisDebugLinesToGpu();
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
    packedSolidWireframeIndexCount = static_cast<uint32_t>(lineI);
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

    packedSolidPatchIndexCount = static_cast<uint32_t>(triIndices.size());

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

    CommitStructurePreviewLinesToGpu();
    CommitAnalysisDebugLinesToGpu();
}

void SceneRenderer::CommitStructurePreviewLinesToGpu()
{
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    const glm::vec4 accentRgba = Color::GetAccent(1, 1.0f);
    const glm::vec3 accentRgb(accentRgba.x, accentRgba.y, accentRgba.z);
    AppendPreviewLineSegments(structurePreviewSegments, accentRgb, verts, idx);
    renderer.UploadStructurePreviewLineMesh(verts, idx);
}

void SceneRenderer::CommitAnalysisDebugLinesToGpu()
{
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    const glm::vec4 accentRgba = Color::GetAccent(1, 1.0f);
    const glm::vec3 accentRgb(accentRgba.x, accentRgba.y, accentRgba.z);
    AppendPreviewLineSegments(analysisDebugSegments, accentRgb, verts, idx);
    renderer.UploadAnalysisDebugLineMesh(verts, idx);
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

void SceneRenderer::UploadPickHighlightMesh(const std::vector<Vertex> &vertices,
                                            const std::vector<uint32_t> &indices,
                                            uint32_t xrayIndexCount)
{
    renderer.UploadPickHighlightMesh(vertices, indices, xrayIndexCount);
}

void SceneRenderer::UploadPickHighlightLineMesh(const std::vector<Vertex> &vertices,
                                                const std::vector<uint32_t> &indices,
                                                uint32_t xrayIndexCount)
{
    renderer.UploadPickHighlightLineMesh(vertices, indices, xrayIndexCount);
}

void SceneRenderer::UploadCalibHoverSpanLineMesh(const std::vector<Vertex> &vertices,
                                                 const std::vector<uint32_t> &indices)
{
    renderer.UploadCalibHoverSpanLineMesh(vertices, indices);
}

void SceneRenderer::UploadOpenBoundaryBlameLineMesh(const std::vector<Vertex> &vertices,
                                                    const std::vector<uint32_t> &indices)
{
    renderer.UploadOpenBoundaryBlameLineMesh(vertices, indices);
}

void SceneRenderer::UploadOpenBoundaryBlameFaceMesh(const std::vector<Vertex> &vertices,
                                                    const std::vector<uint32_t> &indices)
{
    renderer.UploadOpenBoundaryBlameFaceMesh(vertices, indices);
}

void SceneRenderer::UploadPickHighlightRejectMesh(const std::vector<Vertex> &vertices,
                                                  const std::vector<uint32_t> &indices)
{
    renderer.UploadPickHighlightRejectMesh(vertices, indices);
}

void SceneRenderer::UploadPickHighlightCalibInvalidMesh(const std::vector<Vertex> &vertices,
                                                        const std::vector<uint32_t> &indices)
{
    renderer.UploadPickHighlightCalibInvalidMesh(vertices, indices);
}

void SceneRenderer::RenderPickHighlight()
{
    renderer.DrawPickHighlight();
}

void SceneRenderer::RenderPickHighlightCalibInvalid()
{
    renderer.DrawPickHighlightCalibInvalid();
}

void SceneRenderer::RenderPickHighlightReject()
{
    renderer.DrawPickHighlightReject();
}

void SceneRenderer::RenderPickHighlightXray()
{
    renderer.DrawPickHighlight(true);
}

void SceneRenderer::RenderPickHighlightLines(float lineWidthPx)
{
    renderer.DrawPickHighlightLines(lineWidthPx);
}

void SceneRenderer::RenderPickHighlightLinesXray(float lineWidthPx)
{
    renderer.DrawPickHighlightLines(lineWidthPx, true);
}

void SceneRenderer::RenderCalibHoverSpanLine(float lineWidthPx, bool xrayOverlay)
{
    renderer.DrawCalibHoverSpanLine(lineWidthPx, xrayOverlay);
}

void SceneRenderer::RenderOpenBoundaryBlameLine(float lineWidthPx, bool xrayOverlay)
{
    renderer.DrawOpenBoundaryBlameLine(lineWidthPx, xrayOverlay);
}

void SceneRenderer::RenderOpenBoundaryBlameFace(bool xrayOverlay)
{
    renderer.DrawOpenBoundaryBlameFace(xrayOverlay);
}

void SceneRenderer::SetCamera(Camera &camera)
{

    renderer.SetViewMatrix(camera.GetViewMatrix());
    renderer.SetProjectionMatrix(
        ProjectionDepthMode::EffectiveProjection(camera.GetProjectionMatrix()));
    renderer.SetModelMatrix(glm::mat4(1.0f));
    renderer.SetViewPos(camera.GetPosition());
    renderer.SetFogRange(camera.distance);
    // Snapped principal views make front/back edges nearly overlap on screen; an extra forward
    // wire nudge can pull genuinely hidden edges through thin faces.
    renderer.SetWireframeDepthNudgeScale(camera.IsPrincipalAxisView() ? 0.0f : 1.0f);
}

void SceneRenderer::RenderPatches()
{
    const bool useShell = structureViewTranslucentSolidEnabled && packedSolidPatchIndexCount > 0;
    renderer.SetStructureViewSolidTranslucent(useShell, packedSolidPatchIndexCount, structureViewTranslucentSolidAlpha);
    renderer.DrawTriangles();
}

void SceneRenderer::RenderWireframe()
{
    renderer.DrawLines();
}

void SceneRenderer::RenderSolidWireframeOccludedOverlay()
{
    if (packedSolidWireframeIndexCount == 0)
        return;
    renderer.DrawSolidWireframePrefixBehindDepth(packedSolidWireframeIndexCount);
}

void SceneRenderer::RenderStructurePreviewLines(float lineWidthPx)
{
    renderer.DrawStructurePreviewLines(lineWidthPx, false);
    renderer.DrawStructurePreviewLines(std::max(1.0f, lineWidthPx - 1.25f), true);
}

void SceneRenderer::RenderAnalysisDebugLines(float lineWidthPx)
{
    renderer.DrawAnalysisDebugLines(lineWidthPx, false);
    renderer.DrawAnalysisDebugLines(std::max(1.0f, lineWidthPx - 1.25f), true);
}

void SceneRenderer::SetAnalysisDebugLayerDiff(const std::vector<std::vector<glm::dvec3>> &rings, float z)
{
    using Point2D = std::array<double, 2>;
    static constexpr glm::vec4 kDiffColor{1.0f, 0.35f, 0.1f, 0.40f};

    std::vector<AnalysisTriVertex> verts;
    std::vector<uint32_t> indices;

    for (const auto &ring : rings)
    {
        if (ring.size() < 3)
            continue;

        std::vector<Point2D> loop2D;
        loop2D.reserve(ring.size());
        for (const auto &p : ring)
            loop2D.push_back({p.x, p.y});

        // Enforce CCW winding for earcut
        double area = 0.0;
        for (size_t j = 0; j < loop2D.size(); j++)
        {
            size_t k = (j + 1) % loop2D.size();
            area += loop2D[j][0] * loop2D[k][1] - loop2D[k][0] * loop2D[j][1];
        }
        std::vector<glm::dvec3> ordered = ring;
        if (area < 0.0)
        {
            std::reverse(loop2D.begin(), loop2D.end());
            std::reverse(ordered.begin(), ordered.end());
        }

        std::vector<std::vector<Point2D>> polygon{loop2D};
        auto tri = mapbox::earcut<uint32_t>(polygon);
        if (tri.empty())
            continue;

        const uint32_t base = static_cast<uint32_t>(verts.size());
        for (const auto &p : ordered)
            verts.push_back({glm::vec3(p.x, p.y, static_cast<float>(z)), kDiffColor});
        for (uint32_t idx : tri)
            indices.push_back(base + idx);
    }

    renderer.UploadAnalysisDebugTriMesh(verts, indices);
}

void SceneRenderer::RenderAnalysisDebugTriangles()
{
    renderer.DrawAnalysisDebugTriangles();
}

void SceneRenderer::SetOverhangOverlays(const AnalysisResults *results)
{
    std::vector<AnalysisTriVertex> verts;
    std::vector<uint32_t> indices;

    if (results)
    {
        const glm::vec4 color = Color::GetFace(FaceFlawKind::OVERHANG);
        for (const auto &[solid, flaws] : results->faceFlawRanges)
        {
            for (const auto &ff : flaws)
            {
                if (ff.flaw != FaceFlawKind::OVERHANG || !ff.face || !ff.face->HasGeometry())
                    continue;
                OverlayTriangulateFaceZClipped(ff.face, ff.bounds, color, verts, indices);
            }
        }
    }

    renderer.UploadOverhangOverlayMesh(verts, indices);
}

void SceneRenderer::RenderOverhangOverlays()
{
    renderer.DrawOverhangOverlays();
}

void SceneRenderer::SetSharpCornerEdges(const AnalysisResults *results)
{
    sharpCornerEdgeSegments.clear();
    if (results)
    {
        for (const auto &[solid, flaws] : results->faceFlawRanges)
        {
            for (const auto &ff : flaws)
            {
                if (ff.flaw != FaceFlawKind::SHARP_CORNER || ff.clipBoundary.size() != 2)
                    continue;
                sharpCornerEdgeSegments.push_back({glm::vec3(ff.clipBoundary[0]),
                                                      glm::vec3(ff.clipBoundary[1])});
            }
        }
    }

    const glm::vec3 color = glm::vec3(Color::GetFace(FaceFlawKind::SHARP_CORNER));
    std::vector<Vertex> verts;
    std::vector<uint32_t> indices;
    AppendPreviewLineSegments(sharpCornerEdgeSegments, color, verts, indices);
    renderer.UploadAnalysisFlawEdgeMesh(verts, indices);
}

void SceneRenderer::RenderSharpCornerEdges(float lineWidthPx)
{
    renderer.DrawAnalysisFlawEdges(lineWidthPx);
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