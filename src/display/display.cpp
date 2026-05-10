#include "display.hpp"
#include "imgui_internal.h"
#include "rendering/color.hpp"
#include <algorithm>
#include "rendering/UIRenderer/UIStyle.hpp"
#include "rendering/UIRenderer/Icons.hpp"
#include "rendering/UIRenderer/ToolPanel.hpp"
#include "logic/Analysis/Analysis.hpp"
#include "logic/Analysis/Overhang/Overhang.hpp"
#include "logic/Analysis/SharpCorner/SharpCorner.hpp"
#include "logic/Analysis/SmallFeature/SmallFeature.hpp"
#include "logic/Analysis/ThinSection/ThinSection.hpp"
#include "logic/Import/STLImport.hpp"
#include "utils/SystemAccent.hpp"
#include "utils/SystemAppearance.hpp"

#include <filesystem>
#include <mutex>
#include <unordered_set>
#include <vector>
#include <queue>
#include <cstdio>
#include <random>
#include "logic/Import/OBJImport.hpp"
#include "logic/Import/ThreeMFImport.hpp"
#include "input/FileImport.hpp"
#include "input/Input.hpp"
#include "rendering/ScenePick.hpp"
#include "Geometry/Edge.hpp"
#include "Geometry/Point.hpp"
#include "CalibNominal.hpp"
#include "CalibDistanceType.hpp"
#include "CalibCompensation.hpp"
#include "Structure/StructurePreview.hpp"
#include <cmath>
#include <limits>
#include <chrono>
#include <string_view>

#include "ProjectionDepthMode.hpp"
#include "ViewportDepthExperiments.hpp"
#include "rendering/SceneLighting.hpp"
#include "RenderingExperiments.hpp"
#include "UserTuning.hpp"
#include "scene/scene.hpp"
#include "LengthUnit.hpp"

#include <array>
#include <string>

#include "imgui.h"

namespace
{

constexpr float kCalibSpanLabelNdcEps = 0.004f;

/// Pan axis-snapping (`snapInput`) stays off until movement exceeds this so the first few pixels are not
/// forced into pure horizontal/vertical lanes (same units as `Pan` deltas after mouse sensitivity scale).
constexpr float kPanSnapTravelFloor = 3.5e-4f;

/// Font for custom `imguiContent` rows: matches `UIRenderer` (pixel stack when pushed, else pixel/body).
[[nodiscard]] inline ImFont *FontOrInteractiveRow(const UIRenderer &renderer, ImFont *settingsBodyFont)
{
    if (ImFont *f = ImGui::GetFont())
        return f;
    if (ImFont *f = renderer.GetPixelImFont())
        return f;
    return settingsBodyFont;
}

/// Full-row hit target; clipboard is **value only** when `valueStr` is non-empty, otherwise the label (status text).
void CalibDrawCopyableResultRow(ImDrawList *dl, float x0, float y, float w, float rowH, float pad, ImFont *bodyFont,
                                glm::vec4 tcLabel, glm::vec4 tcValue, const char *label, const char *valueStr,
                                const char *imguiIdSuffix)
{
    std::string clip;
    if (valueStr[0] != '\0')
        clip = valueStr;
    else if (label[0] != '\0')
        clip = label;

    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    ImGui::PushID(imguiIdSuffix);
    ImGui::InvisibleButton("copyRow", ImVec2(w, rowH));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    ImGui::PopID();
    if (clicked && !clip.empty())
        ImGui::SetClipboardText(clip.c_str());
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (hovered)
    {
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        glm::vec4 ac = Color::GetAccent(1, 0.12f, 1.0f);
        dl->AddRectFilled(mn, mx, ImGui::GetColorU32(ImVec4(ac.r, ac.g, ac.b, ac.a)), 4.0f);
    }

    const ImU32 lc = ImGui::GetColorU32(ImVec4(tcLabel.r, tcLabel.g, tcLabel.b, tcLabel.a));
    const ImU32 vc = ImGui::GetColorU32(ImVec4(tcValue.r, tcValue.g, tcValue.b, tcValue.a));
    ImFont *drawFont = bodyFont ? bodyFont : ImGui::GetFont();
    const float fs = drawFont ? drawFont->FontSize : ImGui::GetFontSize();
    if (label[0] != '\0')
    {
        const float labelH = drawFont ? drawFont->CalcTextSizeA(fs, FLT_MAX, 0.0f, label).y : ImGui::CalcTextSize(label).y;
        const float ty = y + std::max(0.0f, (rowH - labelH) * 0.5f);
        dl->AddText(drawFont, fs, ImVec2(x0 + pad, ty), lc, label);
    }
    if (valueStr[0] != '\0')
    {
        const ImVec2 vs = drawFont ? drawFont->CalcTextSizeA(fs, FLT_MAX, 0.0f, valueStr)
                                    : ImGui::CalcTextSize(valueStr);
        const float valueH = vs.y;
        const float ty = y + std::max(0.0f, (rowH - valueH) * 0.5f);
        dl->AddText(drawFont, fs, ImVec2(x0 + w - pad - vs.x, ty), vc, valueStr);
    }
}

[[nodiscard]] bool CalibSpanNdcInsideVisibleViewport(glm::vec3 ndc)
{
    return ndc.x >= -1.0f - kCalibSpanLabelNdcEps && ndc.x <= 1.0f + kCalibSpanLabelNdcEps &&
           ndc.y >= -1.0f - kCalibSpanLabelNdcEps && ndc.y <= 1.0f + kCalibSpanLabelNdcEps &&
           ndc.z >= -1.0f - kCalibSpanLabelNdcEps && ndc.z <= 1.0f + kCalibSpanLabelNdcEps;
}

/// Midpoint along `p0`–`p1` centered on the longest contiguous fragment visible inside the NDC cube,
/// so labels stay on-screen when the segment crosses clip planes (orthogonal approximation along arc-length).
[[nodiscard]] std::optional<glm::dvec3> CalibHoverSpanLabelWorldAlongViewportVisible(const glm::mat4 &vp,
                                                                                     glm::dvec3 p0,
                                                                                     glm::dvec3 p1)
{
    constexpr int kSegments = 64;
    std::array<bool, static_cast<size_t>(kSegments) + 1> inside{};
    for (int i = 0; i <= kSegments; ++i)
    {
        const double t = static_cast<double>(i) / static_cast<double>(kSegments);
        const glm::dvec3 pw = p0 + (p1 - p0) * t;
        const glm::vec4 clip = vp * glm::vec4(glm::vec3(pw), 1.0f);
        if (std::abs(clip.w) < 1e-8f)
        {
            inside[static_cast<size_t>(i)] = false;
            continue;
        }
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        inside[static_cast<size_t>(i)] = CalibSpanNdcInsideVisibleViewport(ndc);
    }

    int bestLen = 0;
    int bestStart = 0;
    int runStart = -1;
    auto flushRun = [&](int runEndExclusive)
    {
        if (runStart < 0)
            return;
        const int len = runEndExclusive - runStart;
        if (len > bestLen)
        {
            bestLen = len;
            bestStart = runStart;
        }
        runStart = -1;
    };

    for (int i = 0; i <= kSegments; ++i)
    {
        if (inside[static_cast<size_t>(i)])
        {
            if (runStart < 0)
                runStart = i;
        }
        else
            flushRun(i);
    }
    flushRun(kSegments + 1);

    if (bestLen <= 0)
        return std::nullopt;

    const double midSample =
        static_cast<double>(bestStart) + 0.5 * static_cast<double>(bestLen - 1);
    const double tMid =
        glm::clamp(midSample / static_cast<double>(kSegments), 0.0, 1.0);
    return p0 + (p1 - p0) * tMid;
}

[[nodiscard]] std::string FormatCalibSpanMmLabel(float nominalMm)
{
    const double mm = static_cast<double>(nominalMm);
    const double nearestWhole = std::round(mm);
    char buf[64];
    if (std::abs(mm - nearestWhole) < 5e-4)
        std::snprintf(buf, sizeof(buf), "%.0f mm", nearestWhole);
    else
        std::snprintf(buf, sizeof(buf), "%.3f mm", mm);
    return std::string(buf);
}

const char *AnalysisWorkerPhaseTitle(uint32_t phaseId)
{
    switch (phaseId)
    {
    case AnalysisUiPhase::FacePassesSolidFaces:
        return "Analysing solid faces...";
    case AnalysisUiPhase::FacePassesLooseFaces:
        return "Analysing loose faces...";
    case AnalysisUiPhase::SolidAnalyzers:
        return "Analysing solid mesh...";
    case AnalysisUiPhase::EdgeAnalyzers:
        return "Analysing edges...";
    default:
        return "Analysing model...";
    }
}

std::string ImportProgressLabel(const std::string &phase, float progress01)
{
    if (progress01 < 0.0f || progress01 > 1.0f)
        return phase;

    char buf[256];
    snprintf(buf, sizeof(buf), "%s %.0f%%", phase.c_str(), static_cast<double>(progress01 * 100.0f));
    return std::string(buf);
}

std::string ImportPrerequisiteTitle(float progress01)
{
    if (progress01 < 0.0f || progress01 > 1.0f)
        return "Import a file";

    char buf[64];
    snprintf(buf, sizeof(buf), "Import a file: %.0f%%", static_cast<double>(progress01 * 100.0f));
    return std::string(buf);
}

const Face *ResolveCalibFaceForWorkflow(const Face *pickedFace, const Edge *pickedEdge)
{
    if (pickedFace != nullptr)
        return pickedFace;
    if (pickedEdge == nullptr || pickedEdge->dependencies.empty())
        return nullptr;
    const Face *best = nullptr;
    for (Face *fp : pickedEdge->dependencies)
    {
        const Face *f = fp;
        if (best == nullptr || f < best)
            best = f;
    }
    return best;
}

bool CalibSlotHasPick(const Face *f, const Edge *e)
{
    return f != nullptr || e != nullptr;
}

void GatherCalibLayerHoleInnerEdges(const Scene *scene, const glm::dvec3 &buildDir,
                                    std::unordered_set<const Edge *> &layerHoleInnerEdgesOut)
{
    layerHoleInnerEdgesOut.clear();
    if (scene != nullptr)
        CalibrateDistance::RebuildHoleCalibTopology(*scene, buildDir, layerHoleInnerEdgesOut);
}

[[nodiscard]] bool CalibFacePickPassesWallGate(const Face *face, const Edge *edge, const Scene *scene,
                                               double layerHeightMm, const glm::dvec3 &buildDirWorld)
{
    if (face == nullptr)
        return false;
    if (edge != nullptr && scene != nullptr && layerHeightMm > 0.0 &&
        CalibrateDistance::FaceInFirstLayerSlab(face, scene, layerHeightMm, buildDirWorld) &&
        CalibrateDistance::FaceIsLayerCapParallelBuild(face, buildDirWorld))
        return true;
    return CalibrateDistance::FaceNormalPerpendicularToBuild(face, buildDirWorld);
}

/// Squared ray→segment distance threshold (mm²) for snapping Calibrate picks to boundary edges on the
/// first-layer cap only (`PickCalibrateAtPixel`).
constexpr double kCalibEdgePickMaxDistSqMm = 36.0; // 6 mm

bool CalibSecondPickAcceptsHit(const Face *slot1Face, const Edge *slot1Edge,
                               const Face *firstResolved, const Face *hitFace, const Edge *hitEdge,
                               const Scene *scene, double layerHeightMm, const glm::dvec3 &buildDirWorld,
                               const std::unordered_set<const Edge *> &layerHoleInnerEdges)
{
    const Face *cand = ResolveCalibFaceForWorkflow(hitFace, hitEdge);
    if (cand == nullptr)
        return false;
    if (firstResolved == nullptr)
        return true;

    if (slot1Face != nullptr && slot1Edge != nullptr && scene != nullptr && layerHeightMm > 0.0 &&
        CalibrateDistance::FaceInFirstLayerSlab(slot1Face, scene, layerHeightMm, buildDirWorld) &&
        CalibrateDistance::FaceIsLayerCapParallelBuild(slot1Face, buildDirWorld) &&
        CalibrateNominal::EdgeBelongsToFace(slot1Edge, slot1Face))
    {
        if (hitEdge == nullptr)
            return false;
        if (hitFace != slot1Face)
            return false;
        if (!CalibrateNominal::EdgeBelongsToFace(hitEdge, slot1Face))
            return false;
        if (hitEdge == slot1Edge)
            return false;
        return CalibrateNominal::EdgesAreParallelForCalib(slot1Edge, hitEdge);
    }

    if (!CalibrateDistance::FaceNormalPerpendicularToBuild(firstResolved, buildDirWorld) ||
        !CalibrateDistance::FaceNormalPerpendicularToBuild(cand, buildDirWorld))
        return false;

    if (!CalibrateNominal::NormalsAlignedForCalibPick(firstResolved, cand))
        return false;
    if (scene != nullptr)
    {
        const CalibWorkflow w1 =
            CalibrateDistance::ClassifyFace(firstResolved, scene, layerHeightMm, buildDirWorld,
                                            layerHoleInnerEdges);
        const CalibWorkflow w2 =
            CalibrateDistance::ClassifyFace(cand, scene, layerHeightMm, buildDirWorld, layerHoleInnerEdges);
        if (!CalibrateDistance::CalibSecondPickWorkflowsCompatible(w1, w2))
            return false;
    }
    return true;
}

bool AccumulateFaceViewDirection(glm::dvec3 &sum, const Face *face)
{
    if (face == nullptr || face->loops.empty())
        return false;

    glm::dvec3 normal = face->GetSurface().GetNormal();
    const double len = glm::length(normal);
    if (!(len > 1e-12) || !std::isfinite(normal.x))
        return false;

    sum += normal / len;
    return true;
}

std::optional<glm::vec3> NormalizeViewDirection(const glm::dvec3 &sum)
{
    const double len = glm::length(sum);
    if (!(len > 1e-8) || !std::isfinite(sum.x))
        return std::nullopt;
    return glm::vec3(sum / len);
}

} // namespace

namespace
{
// Set false to restore legacy ±100000 ortho depth (wider slab, coarser depth steps).
inline constexpr bool kTightenOrthoClipPlanes = true;

/// Ray `o + h * d` (world axis from origin: `d` is column of V⁻¹ omitted — here `d = V_linear * axis`).
/// Returns max `h >= 0` inside the ortho view slab in view space (symmetric XY, Z in [zLo,zHi]).
static float RayOrthoSlabMaxPositiveH(const glm::vec3 &o, const glm::vec3 &d,
                                      float halfW, float halfH,
                                      float zLo, float zHi)
{
    float tEnter = 0.0f;
    float tExit = 1.0e30f;

    auto clip = [&](float po, float pd, float lo, float hi)
    {
        if (std::fabs(pd) < 1e-12f)
        {
            if (po < lo || po > hi)
                tExit = -1.0f;
            return;
        }
        const float inv = 1.0f / pd;
        float t0 = (lo - po) * inv;
        float t1 = (hi - po) * inv;
        if (t0 > t1)
            std::swap(t0, t1);
        tEnter = std::max(tEnter, t0);
        tExit = std::min(tExit, t1);
    };

    clip(o.x, d.x, -halfW, halfW);
    clip(o.y, d.y, -halfH, halfH);
    clip(o.z, d.z, zLo, zHi);

    if (tExit < tEnter || tExit < 0.0f)
        return 0.0f;
    const float enterClamped = std::max(0.0f, tEnter);
    if (tExit < enterClamped)
        return 0.0f;
    return tExit;
}

/// World-space half-length of axis lines — must match `ViewportRenderer` mesh and ortho clip.
/// Extent follows the ortho frustum along each principal direction from the world origin so axes
/// reach the viewport edges after rotation/pan; still at least the grid diameter for huge grids.
inline float OrthoClipAxisWorldHalfExtent(const Camera &cam)
{
    const float gridReach = Color::GRID_EXTENT * 2.0f;

    const glm::mat4 V = cam.GetViewMatrix();
    const float halfW = cam.orthoSize * std::fabs(cam.aspectRatio);
    const float halfH = cam.orthoSize;
    const float zLo = std::min(cam.nearPlane, cam.farPlane);
    const float zHi = std::max(cam.nearPlane, cam.farPlane);

    const glm::vec4 o4 = V * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    const float ow = std::max(1e-12f, std::fabs(o4.w));
    const glm::vec3 o = glm::vec3(o4) / ow;

    float best = 1.0f;
    for (int ax = 0; ax < 3; ++ax)
    {
        for (float s : {-1.0f, 1.0f})
        {
            glm::vec3 wd(0.0f);
            wd[ax] = s;
            const glm::vec3 d = glm::vec3(V * glm::vec4(wd, 0.0f));
            if (glm::length(d) < 1e-12f)
                continue;
            const float hExit = RayOrthoSlabMaxPositiveH(o, d, halfW, halfH, zLo, zHi);
            best = std::max(best, hExit);
        }
    }

    const float withMargin = best * 1.08f + 2.0f;
    return std::min(std::max(gridReach, withMargin), 1.0e6f);
}

void ApplyOrthoClipFromViewBounds(Camera &camera, Scene *scene, float axisWorldHalfExtent)
{
    if (!kTightenOrthoClipPlanes)
    {
        camera.nearPlane = -100000.0f;
        camera.farPlane = 100000.0f;
        return;
    }

    glm::mat4 V = camera.GetViewMatrix();
    float minZ = std::numeric_limits<float>::infinity();
    float maxZ = -std::numeric_limits<float>::infinity();

    auto addWorld = [&](const glm::vec3 &p)
    {
        glm::vec4 v = V * glm::vec4(p, 1.0f);
        const float w = std::max(1e-12f, std::abs(v.w));
        const float z = v.z / w;
        minZ = std::min(minZ, z);
        maxZ = std::max(maxZ, z);
    };

    if (scene != nullptr)
    {
        for (const auto &pt : scene->points)
            addWorld(glm::vec3(pt.position));

        // Patches tessellate curved edges to vertices that lie outside the segment between endpoints.
        // Include those envelopes in view-space Z so tightened near/far does not clip when zoomed in.
        constexpr int kCurveClipSamplesPerEdge = 24;
        const double denom =
            std::max(1.0, static_cast<double>(std::max(1, kCurveClipSamplesPerEdge)));
        for (const Edge &edge : scene->edges)
        {
            if (edge.startPoint == nullptr || edge.endPoint == nullptr)
                continue;

            for (Point *bp : edge.bridgePoints)
            {
                if (bp != nullptr)
                    addWorld(glm::vec3(bp->position));
            }

            if (edge.curve == nullptr)
                continue;

            const glm::dvec3 edgeStart(edge.startPoint->position);
            const glm::dvec3 edgeEnd(edge.endPoint->position);

            for (int i = 0; i <= kCurveClipSamplesPerEdge; ++i)
            {
                const double t = static_cast<double>(i) / denom;
                const glm::dvec3 p(edge.curve->Evaluate(t, edgeStart, edgeEnd));
                addWorld(glm::vec3(p));
            }
        }
    }

    const float ext = Color::GRID_EXTENT;
    addWorld(glm::vec3(-ext, -ext, 0.0f));
    addWorld(glm::vec3(ext, ext, 0.0f));
    addWorld(glm::vec3(-ext, ext, 0.0f));
    addWorld(glm::vec3(ext, -ext, 0.0f));

    const float ax = std::max(1.0f, axisWorldHalfExtent);
    addWorld(glm::vec3(ax, 0.0f, 0.0f));
    addWorld(glm::vec3(-ax, 0.0f, 0.0f));
    addWorld(glm::vec3(0.0f, ax, 0.0f));
    addWorld(glm::vec3(0.0f, -ax, 0.0f));
    addWorld(glm::vec3(0.0f, 0.0f, ax));
    addWorld(glm::vec3(0.0f, 0.0f, -ax));

    if (!std::isfinite(minZ) || !std::isfinite(maxZ) || minZ >= maxZ)
    {
        camera.nearPlane = -100000.0f;
        camera.farPlane = 100000.0f;
        return;
    }

    const float span = maxZ - minZ;
    // Keep clip bounds conservative so close-up navigation does not clip near surfaces when
    // scene extrema are sparse (e.g. coarse topology vs shaded/tessellated geometry).
    const float pad = std::max(120.0f, span * 0.3f);
    camera.nearPlane = minZ - pad;
    camera.farPlane = maxZ + pad;
}
} // namespace

void Display::ResetAnalysisPipelineTotals()
{
    analysisPipelineDenomTotal = 0;
    analysisWorkerStepsDone.store(0, std::memory_order_relaxed);
    analysisTintStepsDone.store(0, std::memory_order_relaxed);
    analysisWorkerPhaseIdAtomic.store(0, std::memory_order_relaxed);
    analysisTintStepMarkedForRequestId = 0;
    analysisGpuRebuildStepsCache = 0;
}

void Display::RefreshAnalysisPipelineDenominatorFromScene()
{
    if (scene == nullptr)
    {
        analysisPipelineDenomTotal = 0;
        return;
    }

    const uint64_t analyzeSteps = Analysis::Instance().CountAnalyzeSteps(scene);
    const uint64_t gpuTail = 4u;
    const uint64_t gpuSteps = static_cast<uint64_t>(scene->solids.size()) + gpuTail;

    // 1 = queue/hand-off consumed when the worker starts, `analyzeSteps`, 1 = delivering results to renderer,
    // `gpuSteps` = incremental rebuild (solids + loose/repack/upload/pick).
    analysisPipelineDenomTotal = 1 + analyzeSteps + 1 + gpuSteps;
}

void Display::PrimeAnalysisWorkerQueueStepDone()
{
    analysisWorkerStepsDone.store(1, std::memory_order_relaxed);
}

void Display::OnAnalysisWorkerSceneStep(uint32_t phaseId, uint64_t intraSceneStepIndex)
{
    analysisWorkerPhaseIdAtomic.store(phaseId, std::memory_order_relaxed);
    // Queue consumed + in-scene counter: global step = 1 + intraSceneStepIndex (starts at 1 after first bump).
    analysisWorkerStepsDone.store(1u + intraSceneStepIndex, std::memory_order_relaxed);
}

void Display::TryMarkAnalysisTintStepOnce(uint64_t requestId)
{
    if (analysisTintStepMarkedForRequestId == requestId)
        return;
    analysisTintStepMarkedForRequestId = requestId;
    analysisTintStepsDone.fetch_add(1, std::memory_order_relaxed);
}

float Display::SyncViewportAxisForDepthClip()
{
    const float h = OrthoClipAxisWorldHalfExtent(camera);
    if (std::isnan(lastSyncedAxisWorldHalfExtent) ||
        std::abs(h - lastSyncedAxisWorldHalfExtent) >
            std::max(0.5f, 0.015f * std::max(1.0f, h)))
    {
        lastSyncedAxisWorldHalfExtent = h;
        viewportRenderer.SetAxisWorldHalfExtent(h);
        viewportRenderer.RegenerateGrid();
    }
    return h;
}

void Display::SyncGridLayoutFromSettings()
{
    settings.gridCellsAlongAxis = std::clamp(settings.gridCellsAlongAxis, 4.0f, 8192.0f);
    const LengthUnit du = LengthUnitFromIndex(settings.defaultLengthUnit);
    Color::GRID_CELL_SIZE = MillimetersPerUnit(du);
    Color::GRID_EXTENT = 0.5f * settings.gridCellsAlongAxis * Color::GRID_CELL_SIZE;
    lastSyncedAxisWorldHalfExtent = std::numeric_limits<float>::quiet_NaN();
    viewportRenderer.RegenerateGrid();
    (void)SyncViewportAxisForDepthClip();
    renderDirty = true;
}

void Display::ApplyTheme()
{
    bool dark;
    switch (themeMode)
    {
    case ThemeMode::Light:
        dark = false;
        break;
    case ThemeMode::Dark:
        dark = true;
        break;
    default: // ThemeMode::System
        dark = (SDL_GetSystemTheme() != SDL_SYSTEM_THEME_LIGHT);
        break;
    }
    Color::SetAppearance(dark);
    dark ? ImGui::StyleColorsDark() : ImGui::StyleColorsLight();
    uiRenderer.MarkDirty();
    viewportRenderer.RegenerateGrid();
    if (scene && (!scene->solids.empty() || !scene->faces.empty()))
        MarkStyleDirty();
    MarkPickDirty();
}

Display::Display(int16_t width, int16_t height, const char *title) : window(InitWindow(width, height, title)), renderer(GetWindow()), viewportRenderer(GetWindow()), uiRenderer(GetWindow(), "/System/Library/Fonts/SFNS.ttf"), camera(width, height)
{
    scene = &baseScene;
    // Apply system appearance and accent color before any UI is constructed.
    // themeMode defaults to System — SDL_GetSystemTheme() is called inside ApplyTheme().
    {
        float hue, sat;
        if (SystemAccent::GetHueSat(hue, sat))
            Color::SetAccent(hue, sat);
        // Bootstrap: set appearance directly so viewportRenderer gets the right colors before ApplyTheme.
        bool dark = (SDL_GetSystemTheme() != SDL_SYSTEM_THEME_LIGHT);
        Color::SetAppearance(dark);
        viewportRenderer.RegenerateGrid();
    }

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    Color::IsDark() ? ImGui::StyleColorsDark() : ImGui::StyleColorsLight();
    ImFontConfig avenirCfg;
    avenirCfg.FontNo = 4; // Avenir Heavy — used for headers (textDepth >= 3) and as ImGui default
    ImFont *heavyFont = io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Avenir.ttc", 19.0f, &avenirCfg);
    ImFontConfig avenirBookCfg;
    avenirBookCfg.FontNo = 0; // Avenir Book — used for body text (textDepth <= 2)
    ImFont *bodyFont = io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Avenir.ttc", 17.0f, &avenirBookCfg);
    ImFont *pixelFont = io.Fonts->AddFontDefault();
    ImGui_ImplSDL3_InitForOpenGL(window, glContext);
    ImGui_ImplOpenGL3_Init("#version 330");
    uiRenderer.SetPixelImFont(pixelFont);
    uiRenderer.SetBodyImFont(bodyFont);
    uiRenderer.SetHeavyImFont(heavyFont);

    InitUI();
    LoadSettings();
    SDL_AddEventWatch(ResizeEventWatcher, this);

    LOG_VOID("Initialized display");
}

SDL_Window *Display::InitWindow(int16_t width, int16_t height, const char *title)
{
    // `RenderingExperiments::kSdlTrackpadIsTouchOnly`: see comment on that constant (restart required).
    SDL_SetHint(SDL_HINT_TRACKPAD_IS_TOUCH_ONLY,
                RenderingExperiments::kSdlTrackpadIsTouchOnly ? "1" : "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        LOG_FALSE("Failed to initialize SDL: " + std::string(SDL_GetError()));
        return nullptr;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    const int msaaSamples = RenderingExperiments::kGlFramebufferMsaaSamples;
    if (msaaSamples > 0)
    {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, msaaSamples);
    }
    else
    {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
    }

    windowWidth = width;
    windowHeight = height;

    SDL_Window *w = SDL_CreateWindow(title, windowWidth, windowHeight, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!w)
    {
        LOG_FALSE("Failed to create SDL window: " + std::string(SDL_GetError()));
        return nullptr;
    }

    glContext = SDL_GL_CreateContext(w);
    if (!glContext)
    {
        LOG_FALSE("Failed to create GL context: " + std::string(SDL_GetError()));
        return nullptr;
    }

    SDL_GL_MakeCurrent(w, glContext);
    SDL_GL_SetSwapInterval(1);

    return w;
}

bool Display::ResizeEventWatcher(void *userdata, SDL_Event *event)
{
    if (event->type == SDL_EVENT_WINDOW_RESIZED)
    {
        Display *self = static_cast<Display *>(userdata);
        int width = event->window.data1;
        int height = event->window.data2;
        if (height > 0)
        {
            self->SetAspectRatio(width, height);
            self->Render();
        }
    }
    return true;
}

void Display::Shutdown()
{
    SaveSettings();
    SystemAppearance::ClearChangeCallback();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_RemoveEventWatch(ResizeEventWatcher, this);
    uiRenderer.Shutdown();
    viewportRenderer.Shutdown();
    renderer.Shutdown();
    if (glContext)
        SDL_GL_DestroyContext(glContext);
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();
}

void Display::LoadSettings()
{
    Settings loaded;
    if (!loaded.Load(Settings::DefaultPath()))
        return; // No file yet — keep all defaults.

    // Analysis
    overhangAngle = loaded.overhangAngle;
    sharpCornerAngle = loaded.sharpCornerAngle;
    minFeatureSize = loaded.minFeatureSize;
    thinMinWidth = loaded.thinMinWidth;
    layerHeight = loaded.layerHeight;

    // Appearance
    settingsAccentHue = loaded.accentHue;
    settingsAccentSat = loaded.accentSat;
    settingsAccentUseSystem = loaded.accentUseSystem;
    if (!settingsAccentUseSystem)
        Color::SetAccent(settingsAccentHue, settingsAccentSat);

    themeMode = static_cast<ThemeMode>(std::clamp(loaded.themeMode, 0, 2));
    UserTuning::contrast = std::clamp(loaded.contrast, 0.0f, 1.0f);
    UserTuning::DeriveFromContrast();
    Color::SetUiDepthStep(UserTuning::uiDepthStep);
    ApplyTheme();
    MarkStyleDirty();
    MarkPickDirty();

    // Viewport
    settings.gridCellsAlongAxis = loaded.gridCellsAlongAxis;
    settings.gridPlaneTiltMinOpacity = std::clamp(loaded.gridPlaneTiltMinOpacity, 0.0f, 1.0f);
    settings.defaultLengthUnit = std::clamp(loaded.defaultLengthUnit, 0, 3);
    lastSyncedAxisWorldHalfExtent = std::numeric_limits<float>::quiet_NaN();
    SyncGridLayoutFromSettings();
    viewportRenderer.SetGridPlaneTiltMinOpacity(settings.gridPlaneTiltMinOpacity);

    // Navigation
    mouseSensitivity = loaded.mouseSensitivity;
    UserTuning::snap = std::clamp(loaded.snap, 0.0f, 1.0f);
    UserTuning::DeriveFromSnap();

    // Re-run analysis with restored parameters.
    RebuildAnalysis();
    MarkGeometryDirtyAll();

    // Settings UI was built before load — sync pill indices to restored state.
    if (uiAppearanceThemeSelect)
        uiAppearanceThemeSelect->activeIndex = static_cast<int>(themeMode);
    if (uiAppearanceAccentSelect)
        uiAppearanceAccentSelect->activeIndex = settingsAccentUseSystem ? 0 : 1;
    if (uiDefaultLengthUnitSelect)
        uiDefaultLengthUnitSelect->activeIndex = settings.defaultLengthUnit;
    uiRenderer.MarkDirty();
}

void Display::SaveSettings()
{
    settings.overhangAngle = overhangAngle;
    settings.sharpCornerAngle = sharpCornerAngle;
    settings.minFeatureSize = minFeatureSize;
    settings.thinMinWidth = thinMinWidth;
    settings.layerHeight = layerHeight;
    settings.accentHue = settingsAccentHue;
    settings.accentSat = settingsAccentSat;
    settings.accentUseSystem = settingsAccentUseSystem;
    settings.themeMode = static_cast<int>(themeMode);
    settings.contrast = UserTuning::contrast;
    settings.mouseSensitivity = mouseSensitivity;
    settings.snap = UserTuning::snap;
    // defaultLengthUnit is owned by Settings directly when the user changes the Viewport pill.
    settings.Save(Settings::DefaultPath());
}

void Display::RebuildAnalysis()
{
    // One critical section inside Analysis (clear + default analyzers) — avoids nested locks on the pipeline mutex.
    Analysis::Instance().RebuildDefaultAnalyzers(overhangAngle, layerHeight, minFeatureSize, thinMinWidth, sharpCornerAngle);
}

void Display::UpdateCamera()
{
    cameraDirty = true;
    renderDirty = true;
}

void Display::Render()
{
    auto bg = Color::GetBase();
    glClearColor(bg.r, bg.g, bg.b, 1.0f);
    if (RenderingExperiments::kReverseZDepth)
    {
        glClearDepth(0.0);
        glDepthFunc(GL_GEQUAL);
    }
    else
    {
        glClearDepth(1.0);
        glDepthFunc(GL_LEQUAL);
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    RefreshCalibSpanOverlayForViewportRender();

    const bool structureUiActive =
        activeTool == ActiveTool::Structure && uiStructure != nullptr && uiStructure->visible;
    const bool structureShellTranslucent =
        structureUiActive && structureTranslucentShellEnabled && scene != nullptr && !scene->solids.empty();
    const bool structurePreviewStrutsVisible =
        structureUiActive && structurePreviewEnabled && scene != nullptr && !scene->solids.empty();

    // Face culling applies only to filled triangles (patches + pick highlight), not grid/lines.
    glDisable(GL_CULL_FACE);

    const bool cullOpaqueTriangles = ViewportDepthExperiments::IsBackFaceCull() ||
                                   RenderingExperiments::kCullBackFacesOpaquePatches;
    if (cullOpaqueTriangles)
    {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
    }

    // With Structure translucent shell, draw the XY grid early so blending sees reference grid under
    // tilted surfaces; skip the late grid draw to avoid overwriting the shell.
    if (structureShellTranslucent)
    {
        glDisable(GL_STENCIL_TEST);
        viewportRenderer.Render();
    }

    // Mark only the solid surface pixels in the stencil buffer (value = 1).
    // Lines are excluded — their geometry-shader quads extend beyond silhouettes
    // and would bleed into the stencil, incorrectly clipping axes.
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    renderer.SetStructureViewTranslucentSolid(structureShellTranslucent, 0.42f);
    renderer.RenderPatches();
    renderer.RenderPickHighlight();
    if (RenderingExperiments::kCalibrateSecondPickDrawInvalidFacePool)
        renderer.RenderPickHighlightCalibInvalid();
    renderer.RenderPickHighlightReject();
    if (cullOpaqueTriangles)
        glDisable(GL_CULL_FACE);

    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP); // stop writing before lines
    if (!RenderingExperiments::kDebugSkipSceneWireframe)
        renderer.RenderWireframe();
    // Solid hull edges occluded by the translucent shell depth (back edges, etc.).
    if (structureShellTranslucent)
        renderer.RenderSolidWireframeOccludedOverlay();
    // Center-strut preview: foreground + occluded pass (same mesh) so limbs behind the hull read through.
    if (structurePreviewStrutsVisible)
        renderer.RenderStructurePreviewLines(5.25f);

    // Calibrate: thick accent lines for committed edge picks (and any other pick-highlight lines).
    renderer.RenderPickHighlightLines(6.0f);

    // Grid after solid + wireframe: stencil==0 only so lines do not bleed onto filled surfaces;
    // clip Z bias still keeps axes > grid > scene where stencil allows.
    if (!structureShellTranslucent)
        viewportRenderer.Render();

    viewportRenderer.RenderAxes();
    if (structureShellTranslucent)
        viewportRenderer.RenderAxesBehindScene();

    glDisable(GL_STENCIL_TEST);
    // Occluded selection: translucent face tint behind nearer geometry, then line overlay (stronger).
    renderer.RenderPickHighlightXray();
    renderer.RenderPickHighlightLinesXray(4.0f);
    renderer.RenderCalibHoverSpanLine(5.0f, false);
    renderer.RenderCalibHoverSpanLine(4.0f, true);

    // Nominal span label: GL TextRenderer **before** UI mesh so opaque panels occlude it (ImGui always
    // composites above `UIRenderer`'s GL backgrounds).
    if (calibHoverSpanPreviewActive && !calibHoverSpanLabel.empty())
    {
        GLint vpLabel[4];
        glGetIntegerv(GL_VIEWPORT, vpLabel);
        const glm::mat4 vpMatLabel =
            ProjectionDepthMode::EffectiveProjection(camera.GetProjectionMatrix()) * camera.GetViewMatrix();
        const std::optional<glm::dvec3> labelWorld =
            CalibHoverSpanLabelWorldAlongViewportVisible(vpMatLabel, calibHoverSpanP0, calibHoverSpanP1);
        if (labelWorld.has_value())
        {
            const glm::vec4 clip = vpMatLabel * glm::vec4(glm::vec3(*labelWorld), 1.0f);
            if (std::abs(clip.w) > 1e-8f)
            {
                const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                if (ndc.z >= -1.05f && ndc.z <= 1.05f)
                {
                    const float sx =
                        (ndc.x * 0.5f + 0.5f) * static_cast<float>(vpLabel[2]) + static_cast<float>(vpLabel[0]);
                    const float sy = (1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<float>(vpLabel[3]) +
                                     static_cast<float>(vpLabel[1]);
                    glm::vec4 tc = Color::GetUIText(0);
                    constexpr float kCalibSpanLabelLift = 0.06f;
                    tc.r = tc.g = tc.b = std::clamp(tc.r + kCalibSpanLabelLift, 0.0f, 1.0f);
                    uiRenderer.RenderHudGlyphTextCenteredPx(sx, sy, calibHoverSpanLabel, tc,
                                                            glm::vec4(0.0f, 0.0f, 0.0f, 0.48f));
                }
            }
        }
    }

    // Start ImGui frame
    if (pendingFileTabsRebuild)
    {
        pendingFileTabsRebuild = false;
        RebuildFileTabs();
    }

    if (pendingToolSwitch)
    {
        pendingToolSwitch = false;
        ClearPickHover();
        ClearCalibrateFacePicks();
        uiRenderer.MarkDirty();
        const bool showAnalysis = (activeTool == ActiveTool::Analysis);
        const bool showCalibrate = (activeTool == ActiveTool::Calibrate);
        const bool showStructure = (activeTool == ActiveTool::Structure);
        uiAnalysis->visible = showAnalysis;
        uiCalibrate->visible = showCalibrate;
        if (uiStructure)
            uiStructure->visible = showStructure;
        RefreshStructurePreviewForRenderer();
        if (analysisEnabled != showAnalysis)
        {
            analysisEnabled = showAnalysis;
            UpdateScene();
        }
        else
            MarkGeometryDirtyAll();
        uiRenderer.MarkDirty();
        SyncToolbarToolVisualState();
        RefreshUIMinWindowSize();
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    uiRenderer.Render();

    // Finish ImGui frame
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    SDL_GL_SwapWindow(window);
}

void Display::UpdateScene()
{
    MarkGeometryDirtyAll();
}

void Display::ScheduleNode(InvalidationNode node)
{
    const size_t idx = static_cast<size_t>(node);
    if ((scheduledNodes & NodeBit(node)) == 0)
        invalidationStats.scheduled[idx]++;
    scheduledNodes |= NodeBit(node);
}

void Display::MarkStyleDirty()
{
    if (pendingAnalysisTask.has_value())
    {
        pendingAnalysisTask->RequestCancel();
        pendingAnalysisTask.reset();
        pendingAnalysisScene = nullptr;
    }
    pendingAnalysisTint.reset();
    activeAnalysisTintForRebuild.reset();
    activeAnalysisTintIdentityForRebuild = 0;
    analysisRequestId++;

    styleDirty = true;
    ScheduleNode(InvalidationNode::Style);
    ScheduleNode(InvalidationNode::Pick);
    ScheduleNode(InvalidationNode::UI);
    renderDirty = true;
}

void Display::MarkGeometryDirtyAll()
{
    if (pendingAnalysisTask.has_value())
    {
        pendingAnalysisTask->RequestCancel();
        pendingAnalysisTask.reset();
        pendingAnalysisScene = nullptr;
    }
    pendingAnalysisTint.reset();
    activeAnalysisTintForRebuild.reset();
    activeAnalysisTintIdentityForRebuild = 0;
    lastCommittedAnalysisForRecolor.reset();
    analysisRequestId++;

    geometryDirtyAll = true;
    geometryDirtySolids.clear();
    ScheduleNode(InvalidationNode::Geometry);
    ScheduleNode(InvalidationNode::Analysis);
    ScheduleNode(InvalidationNode::Pick);
    ScheduleNode(InvalidationNode::UI);
    renderDirty = true;
}

void Display::MarkGeometryDirtySolid(const Solid *solid)
{
    if (solid == nullptr)
    {
        MarkGeometryDirtyAll();
        return;
    }
    if (pendingAnalysisTask.has_value())
    {
        pendingAnalysisTask->RequestCancel();
        pendingAnalysisTask.reset();
        pendingAnalysisScene = nullptr;
    }
    pendingAnalysisTint.reset();
    activeAnalysisTintForRebuild.reset();
    activeAnalysisTintIdentityForRebuild = 0;
    lastCommittedAnalysisForRecolor.reset();
    analysisRequestId++;

    if (!geometryDirtyAll)
        geometryDirtySolids.insert(solid);
    ScheduleNode(InvalidationNode::Geometry);
    ScheduleNode(InvalidationNode::Analysis);
    ScheduleNode(InvalidationNode::Pick);
    ScheduleNode(InvalidationNode::UI);
    renderDirty = true;
}

void Display::MarkPickDirty()
{
    pickDirty = true;
    ScheduleNode(InvalidationNode::Pick);
    renderDirty = true;
}

void Display::InvalidationSkip(InvalidationNode node)
{
    invalidationStats.skipped[static_cast<size_t>(node)]++;
}

void Display::InvalidationExec(InvalidationNode node)
{
    invalidationStats.executed[static_cast<size_t>(node)]++;
}

void Display::InvalidationGuardrailViolation()
{
    invalidationStats.guardrailViolations++;
}

void Display::ClearScheduledNodes()
{
    scheduledNodes = 0;
}

void Display::RunPickNode()
{
    // Pick mesh refresh must run after geometry/style has fully settled.
    // Never force a sync fallback rebuild from here; geometry/style work owns rebuild cadence.
    if (geometryDirtyAll || !geometryDirtySolids.empty() || styleDirty)
    {
        InvalidationGuardrailViolation();
        InvalidationSkip(InvalidationNode::Pick);
        return;
    }

    if (pickDirty || hoverPickFace != nullptr || hoverPickEdge != nullptr || calibFacePoint1 != nullptr ||
        calibFacePoint2 != nullptr || calibEdgePoint1 != nullptr || calibEdgePoint2 != nullptr)
    {
        RebuildPickHighlightMesh();
        InvalidationExec(InvalidationNode::Pick);
        return;
    }
    InvalidationSkip(InvalidationNode::Pick);
}

void Display::RunUiNode()
{
    InvalidationExec(InvalidationNode::UI);
}

void Display::Frame()
{
    ProcessDeferredImportIfAny();
    ApplyImportProgressSnapshot();
    const bool ranMainThreadApplyTask = mainThreadPipeline.Process(1.5);

    const float axisH = SyncViewportAxisForDepthClip();
    ApplyOrthoClipFromViewBounds(camera, scene, axisH);

    const bool cameraMovedForPick = cameraDirty;

    // Always sync projection + view to GPU: `ApplyOrthoClipFromViewBounds` updates near/far
    // every frame from the current view matrix; previously we only pushed matrices when
    // `cameraDirty`, so clip planes and `OpenGLRenderer` projection could diverge.
    renderer.SetCamera(camera);
    viewportRenderer.SetCamera(camera);
    if (cameraDirty)
        cameraDirty = false;

    if (pendingAnalysisTask.has_value())
    {
        std::optional<AsyncAnalysisResult> analysisReady = pendingAnalysisTask->TryTake();
        if (analysisReady.has_value())
        {
            const uint64_t readyRequestId = analysisReady->requestId;
            pendingAnalysisTask.reset();
            pendingAnalysisScene = nullptr;
            if (analysisReady->ok && !analysisReady->cancelled &&
                analysisReady->scene == scene && analysisReady->requestId == analysisRequestId)
            {
                pendingAnalysisTint = std::move(*analysisReady);
                TryMarkAnalysisTintStepOnce(readyRequestId);
                styleDirty = true;
                ScheduleNode(InvalidationNode::Style);
                ScheduleNode(InvalidationNode::Analysis);
                ScheduleNode(InvalidationNode::Pick);
                ScheduleNode(InvalidationNode::UI);
                renderDirty = true;
            }
        }
    }

    if (analysisEnabled && pendingAnalysisAfterGeometryRebuild && !geometryDirtyAll && geometryDirtySolids.empty() &&
        !styleDirty && !pendingAnalysisTask.has_value() && !activeAnalysisTintForRebuild.has_value())
    {
        pendingAnalysisAfterGeometryRebuild = false;
        const uint64_t requestId = ++analysisRequestId;
        const Scene *sceneForAnalysis = scene;
        pendingAnalysisTask = taskRunner.Submit([this, sceneForAnalysis, requestId](const TaskRunner::CancellationToken &token) -> AsyncAnalysisResult
                                                {
                                                    AsyncAnalysisResult out;
                                                    out.scene = sceneForAnalysis;
                                                    out.requestId = requestId;
                                                    if (token.IsCancellationRequested())
                                                    {
                                                        out.cancelled = true;
                                                        return out;
                                                    }
                                                    PrimeAnalysisWorkerQueueStepDone();
                                                    Analysis::AnalyzeSceneReporter reporter =
                                                        [this](uint32_t phaseId, uint64_t stepIndex)
                                                    { OnAnalysisWorkerSceneStep(phaseId, stepIndex); };
                                                    out.results = Analysis::Instance().AnalyzeScene(sceneForAnalysis, &reporter);
                                                    if (token.IsCancellationRequested())
                                                    {
                                                        out.cancelled = true;
                                                        return out;
                                                    }
                                                    out.ok = true;
                                                    return out;
                                                });
        pendingAnalysisScene = sceneForAnalysis;
    }

    // Keep processing cards synchronized even when no invalidation pass runs
    // (e.g. import busy / queued states while geometry flags are currently clean).
    const bool hasModelNow = !scene->solids.empty() || !scene->faces.empty();
    const bool geometryOrStyleWorkNow = geometryDirtyAll || !geometryDirtySolids.empty() || styleDirty;
    RefreshToolProcessingCards(hasModelNow, geometryOrStyleWorkNow, ranMainThreadApplyTask);

    if (!ranMainThreadApplyTask && (geometryDirtyAll || !geometryDirtySolids.empty() || styleDirty || pickDirty))
    {
        ScheduleNode(InvalidationNode::Geometry);
        if (styleDirty)
            ScheduleNode(InvalidationNode::Style);
        if (geometryDirtyAll || !geometryDirtySolids.empty() || styleDirty)
            ScheduleNode(InvalidationNode::Analysis);
        ScheduleNode(InvalidationNode::Pick);
        ScheduleNode(InvalidationNode::UI);

        bool hasModel = !scene->solids.empty() || !scene->faces.empty();
        const bool activeHasModel = hasModel && !pendingImportTabActive;
        const bool activePendingImport =
            pendingImportTabActive && (importBusy || pendingImportTask.has_value());

        const bool geometryOrStyleWork = geometryDirtyAll || !geometryDirtySolids.empty() || styleDirty;

        // Toggle Analysis panel sections based on model presence
        if (uiImportPara)
            uiImportPara->visible = activePendingImport || !activeHasModel;
        if (uiResult)
            uiResult->visible = activeHasModel && analysisUiScene == scene;
        if (uiVerdict)
            uiVerdict->visible = activeHasModel && analysisUiScene == scene;
        RefreshToolProcessingCards(hasModel, geometryOrStyleWork, ranMainThreadApplyTask);
        uiRenderer.MarkDirty();

        bool geometryRebuildComplete = true;
        bool hasAnalysisThisFrame = false;
        if (analysisEnabled && pendingAnalysisTint.has_value() && pendingAnalysisTint->scene == scene &&
            styleDirty)
        {
            activeAnalysisTintForRebuild.emplace(std::move(pendingAnalysisTint->results));
            activeAnalysisTintIdentityForRebuild = pendingAnalysisTint->requestId;
            pendingAnalysisTint.reset();
            hasAnalysisThisFrame = true;
            if (!geometryDirtyAll && geometryDirtySolids.empty())
                geometryDirtyAll = true;
        }
        else
        {
            // Do not queue another worker while a prior run's tint is still being applied to geometry
            // (activeAnalysisTintForRebuild); otherwise the next frame can submit AnalyzeScene again,
            // pendingAnalysisTask flips on, and Result/Verdict hide right after they were shown.
            const bool shouldLaunchAsyncAnalysis =
                geometryOrStyleWork && analysisEnabled && !skipAnalysisForNextGeometryRebuild &&
                !pendingAnalysisTask.has_value() && !pendingAnalysisTint.has_value() &&
                !activeAnalysisTintForRebuild.has_value();
            if (shouldLaunchAsyncAnalysis && !renderer.FullRebuildInProgress())
            {
                pendingAnalysisAfterGeometryRebuild = false;
                const uint64_t requestId = analysisRequestId;
                const Scene *sceneForAnalysis = scene;
                pendingAnalysisTask = taskRunner.Submit([this, sceneForAnalysis, requestId](const TaskRunner::CancellationToken &token) -> AsyncAnalysisResult
                                                        {
                                                            AsyncAnalysisResult out;
                                                            out.scene = sceneForAnalysis;
                                                            out.requestId = requestId;
                                                            if (token.IsCancellationRequested())
                                                            {
                                                                out.cancelled = true;
                                                                return out;
                                                            }
                                                            PrimeAnalysisWorkerQueueStepDone();
                                                            Analysis::AnalyzeSceneReporter reporter =
                                                                [this](uint32_t phaseId, uint64_t stepIndex)
                                                            { OnAnalysisWorkerSceneStep(phaseId, stepIndex); };
                                                            out.results =
                                                                Analysis::Instance().AnalyzeScene(sceneForAnalysis, &reporter);
                                                            if (token.IsCancellationRequested())
                                                            {
                                                                out.cancelled = true;
                                                                return out;
                                                            }
                                                            out.ok = true;
                                                            return out;
                                                        });
                pendingAnalysisScene = sceneForAnalysis;
            }
            else if (shouldLaunchAsyncAnalysis)
            {
                // Geometry work is still in-flight; launch analysis as soon as the rebuild drains.
                pendingAnalysisAfterGeometryRebuild = true;
            }
        }

        const AnalysisResults *activeTintPtr =
            activeAnalysisTintForRebuild.has_value() ? &*activeAnalysisTintForRebuild : nullptr;
        const uint64_t activeTintId =
            activeAnalysisTintForRebuild.has_value() ? activeAnalysisTintIdentityForRebuild : 0;

        if (geometryDirtyAll || !geometryDirtySolids.empty())
            RefreshStructurePreviewForRenderer();

        if (geometryDirtyAll)
        {
            geometryRebuildComplete =
                renderer.RebuildAllIncremental(scene, activeTintPtr, 2.5, activeTintId);
            InvalidationExec(InvalidationNode::Geometry);
            if (!geometryRebuildComplete)
            {
                // Keep scheduling geometry work until incremental rebuild completes.
                renderDirty = true;
                pickDirty = true;
            }
        }
        else if (!geometryDirtySolids.empty())
        {
            renderer.RebuildSolids(scene, geometryDirtySolids, activeTintPtr);
            InvalidationExec(InvalidationNode::Geometry);
        }
        else if (styleDirty)
        {
            if (hasAnalysisThisFrame)
            {
                // Applying fresh analysis often changes rendered topology enough to force
                // a full rebuild. Route through the same incremental path to avoid a hitch.
                geometryDirtyAll = true;
                geometryRebuildComplete =
                    renderer.RebuildAllIncremental(scene, activeTintPtr, 2.5, activeTintId);
                InvalidationExec(InvalidationNode::Geometry);
                if (!geometryRebuildComplete)
                {
                    renderDirty = true;
                    pickDirty = true;
                }
            }
            else
            {
                const AnalysisResults *recolorPtr = nullptr;
                if (analysisEnabled && lastCommittedAnalysisForRecolor.has_value())
                    recolorPtr = &*lastCommittedAnalysisForRecolor;
                renderer.RecolorOnly(scene, recolorPtr);
                InvalidationExec(InvalidationNode::Style);
            }
        }

        auto stillPickable = [&](const Face *f) -> bool
        {
            if (f == nullptr)
                return true;
            for (const PickTriangle &tri : renderer.GetPickTriangles())
            {
                if (tri.face == f)
                    return true;
            }
            return false;
        };

        auto stillPickableEdge = [&](const Edge *e) -> bool
        {
            if (e == nullptr)
                return true;
            for (const PickSegment &ps : renderer.GetPickSegments())
            {
                if (ps.edge == e)
                    return true;
            }
            return false;
        };

        if (hoverPickFace != nullptr && !stillPickable(hoverPickFace))
        {
            hoverPickFace = nullptr;
            pickDirty = true;
        }
        if (hoverPickEdge != nullptr && !stillPickableEdge(hoverPickEdge))
        {
            hoverPickEdge = nullptr;
            pickDirty = true;
        }

        bool calibPickInvalidated = false;
        if (calibFacePoint1 != nullptr && !stillPickable(calibFacePoint1))
        {
            calibFacePoint1 = nullptr;
            calibStepPoint1 = Icons::StepState::Active;
            calibPickInvalidated = true;
            pickDirty = true;
        }
        if (calibEdgePoint1 != nullptr && !stillPickableEdge(calibEdgePoint1))
        {
            calibEdgePoint1 = nullptr;
            calibStepPoint1 = Icons::StepState::Active;
            calibPickInvalidated = true;
            pickDirty = true;
        }
        if (calibFacePoint2 != nullptr && !stillPickable(calibFacePoint2))
        {
            calibFacePoint2 = nullptr;
            calibStepPoint2 = Icons::StepState::Active;
            calibPickInvalidated = true;
            pickDirty = true;
        }
        if (calibEdgePoint2 != nullptr && !stillPickableEdge(calibEdgePoint2))
        {
            calibEdgePoint2 = nullptr;
            calibStepPoint2 = Icons::StepState::Active;
            calibPickInvalidated = true;
            pickDirty = true;
        }
        if (calibPickInvalidated)
        {
            uiRenderer.MarkDirty();
            RefreshCalibWorkflow();
        }
        else if (CalibSlotHasPick(calibFacePoint1, calibEdgePoint1) &&
                 CalibSlotHasPick(calibFacePoint2, calibEdgePoint2))
        {
            RefreshCalibCompensation();
            uiRenderer.MarkDirty();
        }

        RunPickNode();

        if (hasAnalysisThisFrame)
        {
            AnalysisResults &results = *activeAnalysisTintForRebuild;
            InvalidationExec(InvalidationNode::Analysis);
            // Count flaws per type and push to UI
            size_t thinSections = 0, smallFeatures = 0, sharpEdges = 0;

            // Count overhang regions as connected components of adjacent overhang faces
            // Skip defunct faces (loops cleared by MergeCoplanarFaces)
            std::unordered_set<const Face *> overhangFaces;
            for (const auto &[face, kind] : results.faceFlaws)
            {
                if (kind == FaceFlawKind::OVERHANG && !face->loops.empty())
                    overhangFaces.insert(face);
            }

            size_t overhangs = 0;
            std::unordered_set<const Face *> visited;
            for (const Face *seed : overhangFaces)
            {
                if (visited.count(seed))
                    continue;
                overhangs++;
                std::queue<const Face *> bfs;
                bfs.push(seed);
                visited.insert(seed);
                while (!bfs.empty())
                {
                    const Face *current = bfs.front();
                    bfs.pop();
                    for (const auto &loop : current->loops)
                    {
                        for (const auto &oe : loop)
                        {
                            for (Face *neighbor : oe.edge->dependencies)
                            {
                                if (overhangFaces.count(neighbor) && !visited.count(neighbor))
                                {
                                    visited.insert(neighbor);
                                    bfs.push(neighbor);
                                }
                            }
                        }
                    }
                }
            }

            // Collect thin-section and small-feature faces for BFS grouping
            std::unordered_set<const Face *> thinSectionFaces;
            std::unordered_set<const Face *> smallFeatureFaces;
            for (const auto &[solid, flaws] : results.faceFlawRanges)
            {
                for (const auto &ff : flaws)
                {
                    switch (ff.flaw)
                    {
                    case FaceFlawKind::THIN_SECTION:
                        if (ff.face && !ff.face->loops.empty())
                            thinSectionFaces.insert(ff.face);
                        break;
                    case FaceFlawKind::SMALL_FEATURE:
                        if (ff.face && !ff.face->loops.empty())
                            smallFeatureFaces.insert(ff.face);
                        break;
                    default:
                        break;
                    }
                }
            }

            // Count connected components of adjacent thin-section faces
            {
                std::unordered_set<const Face *> visited;
                for (const Face *seed : thinSectionFaces)
                {
                    if (visited.count(seed))
                        continue;
                    thinSections++;
                    std::queue<const Face *> bfs;
                    bfs.push(seed);
                    visited.insert(seed);
                    while (!bfs.empty())
                    {
                        const Face *current = bfs.front();
                        bfs.pop();
                        for (const auto &loop : current->loops)
                        {
                            for (const auto &oe : loop)
                            {
                                for (Face *neighbor : oe.edge->dependencies)
                                {
                                    if (thinSectionFaces.count(neighbor) && !visited.count(neighbor))
                                    {
                                        visited.insert(neighbor);
                                        bfs.push(neighbor);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Count connected components of adjacent small-feature faces
            {
                std::unordered_set<const Face *> visited;
                for (const Face *seed : smallFeatureFaces)
                {
                    if (visited.count(seed))
                        continue;
                    smallFeatures++;
                    std::queue<const Face *> bfs;
                    bfs.push(seed);
                    visited.insert(seed);
                    while (!bfs.empty())
                    {
                        const Face *current = bfs.front();
                        bfs.pop();
                        for (const auto &loop : current->loops)
                        {
                            for (const auto &oe : loop)
                            {
                                for (Face *neighbor : oe.edge->dependencies)
                                {
                                    if (smallFeatureFaces.count(neighbor) && !visited.count(neighbor))
                                    {
                                        visited.insert(neighbor);
                                        bfs.push(neighbor);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            for (const auto &[solid, edgeVec] : results.edgeFlaws)
            {
                for (const auto &e : edgeVec)
                {
                    if (e.flaw == EdgeFlawKind::SHARP_CORNER)
                        sharpEdges++;
                }
            }

            // Compute 3D bounding boxes per flaw category for click-to-frame
            auto expandBounds = [](glm::vec3 &bMin, glm::vec3 &bMax, const glm::dvec3 &p)
            {
                glm::vec3 fp(p);
                bMin = glm::min(bMin, fp);
                bMax = glm::max(bMax, fp);
            };
            auto expandFaceBounds = [&](glm::vec3 &bMin, glm::vec3 &bMax, const Face *face)
            {
                for (const auto &loop : face->loops)
                    for (const auto &oe : loop)
                    {
                        expandBounds(bMin, bMax, oe.edge->startPoint->position);
                        expandBounds(bMin, bMax, oe.edge->endPoint->position);
                    }
            };

            constexpr float INF = std::numeric_limits<float>::max();
            glm::vec3 overhangMin(INF), overhangMax(-INF);
            glm::vec3 thinMin(INF), thinMax(-INF);
            glm::vec3 smallMin(INF), smallMax(-INF);
            glm::vec3 sharpMin(INF), sharpMax(-INF);
            glm::dvec3 overhangViewDir(0.0);
            glm::dvec3 thinViewDir(0.0);
            glm::dvec3 smallViewDir(0.0);
            glm::dvec3 sharpViewDir(0.0);

            // Overhang face bounds
            for (const Face *face : overhangFaces)
            {
                expandFaceBounds(overhangMin, overhangMax, face);
                AccumulateFaceViewDirection(overhangViewDir, face);
            }

            // Thin section / small feature bounds from faceFlawRanges
            for (const auto &[solid, flaws] : results.faceFlawRanges)
            {
                for (const auto &ff : flaws)
                {
                    if (ff.flaw == FaceFlawKind::THIN_SECTION && ff.face)
                    {
                        expandFaceBounds(thinMin, thinMax, ff.face);
                        AccumulateFaceViewDirection(thinViewDir, ff.face);
                    }
                    else if (ff.flaw == FaceFlawKind::SMALL_FEATURE && ff.face)
                    {
                        expandFaceBounds(smallMin, smallMax, ff.face);
                        AccumulateFaceViewDirection(smallViewDir, ff.face);
                    }
                }
            }

            // Sharp edge bounds
            for (const auto &[solid, edgeVec] : results.edgeFlaws)
            {
                for (const auto &e : edgeVec)
                {
                    if (e.flaw == EdgeFlawKind::SHARP_CORNER && e.edge)
                    {
                        expandBounds(sharpMin, sharpMax, e.edge->startPoint->position);
                        expandBounds(sharpMin, sharpMax, e.edge->endPoint->position);
                        for (const Face *face : e.edge->dependencies)
                            AccumulateFaceViewDirection(sharpViewDir, face);
                    }
                }
            }

            // Bright versions of flaw colors for UI text
            glm::vec4 overhangColor = glm::vec4(Color::GetFace(FaceFlawKind::OVERHANG).r + 0.4f,
                                                Color::GetFace(FaceFlawKind::OVERHANG).g + 0.2f,
                                                Color::GetFace(FaceFlawKind::OVERHANG).b + 0.2f, 1.0f);
            glm::vec4 thinColor = glm::vec4(Color::GetFace(FaceFlawKind::THIN_SECTION).r + 0.4f,
                                            Color::GetFace(FaceFlawKind::THIN_SECTION).g + 0.3f,
                                            Color::GetFace(FaceFlawKind::THIN_SECTION).b + 0.15f, 1.0f);
            glm::vec4 edgeColor = glm::vec4(Color::GetEdge(EdgeFlawKind::SHARP_CORNER).r + 0.3f,
                                            Color::GetEdge(EdgeFlawKind::SHARP_CORNER).g + 0.1f,
                                            Color::GetEdge(EdgeFlawKind::SHARP_CORNER).b + 0.1f, 1.0f);

            auto makeFrameCallback = [this](glm::vec3 bMin, glm::vec3 bMax,
                                            std::optional<glm::vec3> cameraBackDirection) -> std::function<void()>
            {
                if (bMin.x > bMax.x)
                    return nullptr; // no valid bounds
                return [this, bMin, bMax, cameraBackDirection]()
                {
                    if (cameraBackDirection)
                        camera.FrameBoundsFromDirection(bMin, bMax, *cameraBackDirection);
                    else
                        camera.FrameBounds(bMin, bMax);
                    cameraDirty = true;
                    renderDirty = true;
                };
            };

            // Write live flaw state — read each frame by the imguiContent lambdas in uiResult
            flawOverhang.count = overhangs;
            flawOverhang.frameCallback =
                makeFrameCallback(overhangMin, overhangMax, NormalizeViewDirection(overhangViewDir));
            flawSharp.count = sharpEdges;
            flawSharp.frameCallback = makeFrameCallback(sharpMin, sharpMax, NormalizeViewDirection(sharpViewDir));
            flawThin.count = thinSections;
            flawThin.frameCallback = makeFrameCallback(thinMin, thinMax, NormalizeViewDirection(thinViewDir));
            flawSmall.count = smallFeatures;
            flawSmall.frameCallback = makeFrameCallback(smallMin, smallMax, NormalizeViewDirection(smallViewDir));

            // Log analysis results to session
            {
                auto &sl = SessionLogger::Instance();
                sl.state.overhangs = overhangs;
                sl.state.sharpEdges = sharpEdges;
                sl.state.thinSections = thinSections;
                sl.state.smallFeatures = smallFeatures;
                sl.LogAnalysisRun();
            }

            // Two-tier verdict
            bool hasVisual = (overhangs > 0) || (thinSections > 0);
            bool hasPrecision = (smallFeatures > 0) || (sharpEdges > 0);

            glm::vec4 passColor = glm::vec4(0.4f, 0.8f, 0.4f, 1.0f);
            glm::vec4 failColor = glm::vec4(0.9f, 0.4f, 0.4f, 1.0f);

            std::vector<SectionLine> verdictLines;
            if (hasVisual && hasPrecision)
                verdictLines.push_back({"Some areas might not print well or accurately", "", failColor});
            else if (hasVisual)
                verdictLines.push_back({"Some areas might not print well", "", failColor});
            else if (hasPrecision)
                verdictLines.push_back({"Some areas might not print accurately", "", failColor});
            else
            {
                verdictLines.push_back({"No issues detected", "", passColor});

                // Contextual printing tip based on model geometry
                glm::dvec3 modelMin(std::numeric_limits<double>::max());
                glm::dvec3 modelMax(std::numeric_limits<double>::lowest());
                size_t totalLoops = 0;

                for (const auto &face : scene->faces)
                {
                    totalLoops += face.loops.size();
                    for (const auto &loop : face.loops)
                        for (const auto &oe : loop)
                        {
                            const auto &p = oe.edge->startPoint->position;
                            modelMin = glm::min(modelMin, p);
                            modelMax = glm::max(modelMax, p);
                        }
                }

                double height = modelMax.z - modelMin.z;
                double footprintX = modelMax.x - modelMin.x;
                double footprintY = modelMax.y - modelMin.y;
                double footprintArea = footprintX * footprintY;
                double footprintDiag = std::sqrt(footprintX * footprintX + footprintY * footprintY);

                struct Tip
                {
                    const char *text;
                    float weight;
                };
                std::vector<Tip> tips = {
                    {"Remember to clean your build plate!", 1.0f},
                    {"A brim can help with bed adhesion", 1.0f},
                    {"Keep your filament dry", 1.0f},
                    {"Level your bed before printing", 1.0f},
                    {"Check your nozzle for wear", 0.5f},
                };

                // Tall & narrow → adhesion tips
                if (footprintDiag > 0 && height / footprintDiag > 1.5)
                {
                    tips[0].weight += 3.0f; // clean build plate
                    tips[1].weight += 3.0f; // brim
                }

                // Large footprint → level bed matters more
                if (footprintArea > 2500.0) // > ~50x50 mm
                    tips[3].weight += 3.0f;

                // Only re-roll the tip when transitioning from flawed → pass
                if (!lastVerdictWasPass)
                {
                    float totalWeight = 0;
                    for (const auto &t : tips)
                        totalWeight += t.weight;

                    static std::mt19937 rng(std::random_device{}());
                    std::uniform_real_distribution<float> dist(0.0f, totalWeight);
                    float r = dist(rng);
                    cachedTip = tips[0].text;
                    float cumulative = 0;
                    for (const auto &t : tips)
                    {
                        cumulative += t.weight;
                        if (r < cumulative)
                        {
                            cachedTip = t.text;
                            break;
                        }
                    }
                }

                glm::vec4 tipColor(0.55f, 0.55f, 0.55f, 1.0f);
                verdictLines.push_back({cachedTip, "", tipColor});
            }

            bool verdictIsPass = !hasVisual && !hasPrecision;
            lastVerdictWasPass = verdictIsPass;
            if (uiVerdict)
                uiVerdict->values = std::move(verdictLines);
            analysisUiScene = scene;
        }
        else
        {
            InvalidationSkip(InvalidationNode::Analysis);
            // Only clear live analysis UI when analysis is off. When analysis is on, geometry/style
            // can stay dirty across many frames (incremental rebuild); clearing here would erase
            // counts/verdict the frame after async results were applied.
            if (geometryOrStyleWork && !analysisEnabled)
            {
                lastVerdictWasPass = false;
                flawOverhang = {};
                flawSharp = {};
                flawThin = {};
                flawSmall = {};
                if (uiVerdict)
                    uiVerdict->values = {};
            }
        }
        if (geometryOrStyleWork && skipAnalysisForNextGeometryRebuild)
        {
            // Import handoff: keep the first rebuild responsive, then request one
            // follow-up style/analysis pass once geometry is visible.
            skipAnalysisForNextGeometryRebuild = false;
            pendingAnalysisAfterGeometryRebuild = true;
        }
        if (geometryRebuildComplete)
        {
            geometryDirtyAll = false;
            geometryDirtySolids.clear();
            styleDirty = false;
            pickDirty = false;
            if (activeAnalysisTintForRebuild.has_value())
            {
                lastCommittedAnalysisForRecolor = std::move(*activeAnalysisTintForRebuild);
                activeAnalysisTintForRebuild.reset();
            }
            else
            {
                lastCommittedAnalysisForRecolor.reset();
                activeAnalysisTintForRebuild.reset();
            }
            activeAnalysisTintIdentityForRebuild = 0;
        }
        else
        {
            // Leave dirty flags latched so the next frame continues incremental rebuild.
            geometryDirtyAll = true;
        }
        // Re-sync cards after tint consumption / verdict fill (first Refresh in this block ran before that work).
        RefreshToolProcessingCards(hasModel, geometryOrStyleWork, ranMainThreadApplyTask);
        InvalidationExec(InvalidationNode::UI);
        ClearScheduledNodes();
    }

    if (cameraMovedForPick)
    {
        float mx, my;
        SDL_GetMouseState(&mx, &my);
        UpdatePickHover(mx, my);
    }

    if (importBusy || pendingImportTask.has_value())
        renderDirty = true;

    if (renderDirty)
    {
        Render();
        renderDirty = false;
    }
}

void Display::SetAspectRatio(const uint16_t width, const uint16_t height)
{
    windowWidth = static_cast<int16_t>(width);
    windowHeight = static_cast<int16_t>(height);

    // Use physical pixels for the GL viewport so Retina/HiDPI framebuffers
    // are covered correctly. Logical dimensions are still used for the camera
    // and UI (aspect ratio is identical; UI uses its own coordinate space).
    int physW, physH;
    SDL_GetWindowSizeInPixels(window, &physW, &physH);
    glViewport(0, 0, physW, physH);

    camera.SetAspectRatio(static_cast<float>(width) / static_cast<float>(std::max<uint16_t>(1, height)),
                          width, height);
    uiRenderer.SetScreenSize(width, height);

    const float axisH = SyncViewportAxisForDepthClip();
    ApplyOrthoClipFromViewBounds(camera, scene, axisH);

    // Push updated matrices to the renderers immediately. ResizeEventWatcher
    // calls Render() before Frame() has a chance to process cameraDirty, so
    // the renderer must have the fresh projection before that Render() runs.
    renderer.SetCamera(camera);
    viewportRenderer.SetCamera(camera);

    renderDirty = true;
}

void Display::Zoom(const float offsetY, const glm::vec3 &posCursotr)
{
    camera.Zoom(offsetY, posCursotr);

    UpdateCamera();
}

glm::vec3 Display::ScreenToWorld(float pixelX, float pixelY) const
{
    int w, h;
    SDL_GetWindowSize(window, &w, &h);

    float ndcX = 2.0f * pixelX / w - 1.0f;
    float ndcY = 1.0f - 2.0f * pixelY / h;

    glm::vec3 right = camera.orientation * glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 up = camera.orientation * glm::vec3(0.0f, 1.0f, 0.0f);

    return camera.target + right * ndcX * camera.orthoSize * camera.aspectRatio + up * ndcY * camera.orthoSize;
}

PickFilter Display::GetActivePickFilter() const
{
    if (activeTool != ActiveTool::Calibrate)
        return PickFilter::None;
    if (!calibPara_Point1 || !calibPara_Point1->visible)
        return PickFilter::None;
    if (!scene || (scene->solids.empty() && scene->faces.empty()))
        return PickFilter::None;

    const bool awaitingPoint1Pick = calibPara_Point1->selected;
    const bool awaitingPoint2Pick =
        calibPara_Point2 && calibPara_Point2->selected;
    if (!awaitingPoint1Pick && !awaitingPoint2Pick)
        return PickFilter::None;

    return PickFilter::Faces;
}

void Display::ClearPickHover()
{
    hoverPickFace = nullptr;
    hoverPickEdge = nullptr;
    hoverCalibPickRejected = false;
    MarkPickDirty();
}

void Display::ClearCalibrateFacePicks()
{
    calibFacePoint1 = nullptr;
    calibFacePoint2 = nullptr;
    calibEdgePoint1 = nullptr;
    calibEdgePoint2 = nullptr;
    calibStepPoint1 = Icons::StepState::Active;
    calibStepPoint2 = Icons::StepState::Active;
    if (calibPara_Point1)
        calibPara_Point1->selected = calibPara_Point1->visible;
    if (calibPara_Point2)
        calibPara_Point2->selected = false;
    RefreshCalibWorkflow();
    RefreshCalibDerivedRowVisible();
    uiRenderer.MarkDirty();
    MarkPickDirty();
}

void Display::SetHoverCalibPick(const Face *face, const Edge *edge, bool rejected)
{
    if (hoverPickFace == face && hoverPickEdge == edge && hoverCalibPickRejected == rejected)
    {
        if (face != nullptr || edge != nullptr)
            return;
        if (calibFacePoint1 != nullptr || calibFacePoint2 != nullptr || calibEdgePoint1 != nullptr ||
            calibEdgePoint2 != nullptr)
            return;
        if (pickHighlightIndices.empty() && pickHighlightRejectIndices.empty() &&
            pickHighlightCalibInvalidIndices.empty())
            return;
    }
    hoverPickFace = face;
    hoverPickEdge = edge;
    hoverCalibPickRejected = rejected;
    MarkPickDirty();
}

void Display::RebuildPickHighlightMesh()
{
    pickHighlightVertices.clear();
    pickHighlightIndices.clear();
    pickHighlightLineVertices.clear();
    pickHighlightLineIndices.clear();
    pickHighlightRejectVertices.clear();
    pickHighlightRejectIndices.clear();
    pickHighlightCalibInvalidVertices.clear();
    pickHighlightCalibInvalidIndices.clear();

    const std::vector<PickTriangle> &tris = renderer.GetPickTriangles();
    uint32_t nextVert = 0;

    auto appendFaceTrisSolid = [&](const Face *face, const glm::vec3 &rgb)
    {
        if (face == nullptr)
            return;
        for (const PickTriangle &tri : tris)
        {
            if (tri.face != face)
                continue;
            const glm::dvec3 e1 = tri.v1 - tri.v0;
            const glm::dvec3 e2 = tri.v2 - tri.v0;
            glm::vec3 n = glm::normalize(glm::vec3(glm::cross(e1, e2)));
            if (!std::isfinite(static_cast<double>(n.x)) || glm::length(n) < 1e-6f)
                n = glm::vec3(0.0f, 0.0f, 1.0f);

            pickHighlightVertices.push_back({glm::vec3(tri.v0), rgb, n});
            pickHighlightVertices.push_back({glm::vec3(tri.v1), rgb, n});
            pickHighlightVertices.push_back({glm::vec3(tri.v2), rgb, n});
            pickHighlightIndices.push_back(nextVert);
            pickHighlightIndices.push_back(nextVert + 1);
            pickHighlightIndices.push_back(nextVert + 2);
            nextVert += 3;
        }
    };

    auto appendFaceTris = [&](const Face *face, float accentDepthSteps, float satMult)
    {
        if (face == nullptr)
            return;
        const glm::vec3 accent = glm::vec3(Color::GetAccentSteps(accentDepthSteps, 1.0f, satMult));
        for (const PickTriangle &tri : tris)
        {
            if (tri.face != face)
                continue;
            const glm::dvec3 e1 = tri.v1 - tri.v0;
            const glm::dvec3 e2 = tri.v2 - tri.v0;
            glm::vec3 n = glm::normalize(glm::vec3(glm::cross(e1, e2)));
            if (!std::isfinite(static_cast<double>(n.x)) || glm::length(n) < 1e-6f)
                n = glm::vec3(0.0f, 0.0f, 1.0f);

            pickHighlightVertices.push_back({glm::vec3(tri.v0), accent, n});
            pickHighlightVertices.push_back({glm::vec3(tri.v1), accent, n});
            pickHighlightVertices.push_back({glm::vec3(tri.v2), accent, n});
            pickHighlightIndices.push_back(nextVert);
            pickHighlightIndices.push_back(nextVert + 1);
            pickHighlightIndices.push_back(nextVert + 2);
            nextVert += 3;
        }
    };

    // Face-only committed picks get the face tint. Edge-snapped picks store the owning face for
    // geometry logic but drawing both reads as "face selected" (hover already suppresses face fill
    // when hoverPickEdge is set — mirror that for committed calibEdgePoint1/2).
    if (calibFacePoint1 != nullptr && calibEdgePoint1 == nullptr)
        appendFaceTris(calibFacePoint1, 1.0f, 0.72f);
    if (calibFacePoint2 != nullptr && calibEdgePoint2 == nullptr)
        appendFaceTris(calibFacePoint2, 1.0f, 0.72f);

    const std::vector<PickSegment> &segPick = renderer.GetPickSegments();
    const glm::vec3 lineNormal(0.0f, 0.0f, 1.0f);
    auto appendEdgeLinesRgb = [&](const Edge *edge, const glm::vec3 &rgb)
    {
        if (edge == nullptr)
            return;
        for (const PickSegment &ps : segPick)
        {
            if (ps.edge != edge)
                continue;
            const uint32_t base = static_cast<uint32_t>(pickHighlightLineVertices.size());
            pickHighlightLineVertices.push_back({glm::vec3(ps.v0), rgb, lineNormal});
            pickHighlightLineVertices.push_back({glm::vec3(ps.v1), rgb, lineNormal});
            pickHighlightLineIndices.push_back(base);
            pickHighlightLineIndices.push_back(base + 1);
        }
    };
    auto appendEdgeLines = [&](const Edge *edge, float accentDepthSteps, float satMult)
    {
        if (edge == nullptr)
            return;
        appendEdgeLinesRgb(edge, glm::vec3(Color::GetAccentSteps(accentDepthSteps, 1.0f, satMult)));
    };

    appendEdgeLines(calibEdgePoint1, 1.0f, 0.72f);
    appendEdgeLines(calibEdgePoint2, 1.0f, 0.72f);

    const bool calibSecondPickConstrained =
        activeTool == ActiveTool::Calibrate && calibPara_Point2 && calibPara_Point2->selected &&
        CalibSlotHasPick(calibFacePoint1, calibEdgePoint1);
    const Face *firstForInvalidPool =
        calibSecondPickConstrained ? ResolveCalibFaceForWorkflow(calibFacePoint1, calibEdgePoint1) : nullptr;
    if (firstForInvalidPool != nullptr && RenderingExperiments::kCalibrateSecondPickDrawInvalidFacePool)
    {
        std::unordered_set<const Edge *> layerHoleInnerEdges;
        const glm::dvec3 calibBuildDir = CalibrateDistance::DefaultCalibrateBuildDirection();
        GatherCalibLayerHoleInnerEdges(scene, calibBuildDir, layerHoleInnerEdges);
        const double layerMm = static_cast<double>(layerHeight);

        const bool elephantEdgeSecondPickMode =
            calibEdgePoint1 != nullptr && scene != nullptr &&
            CalibrateDistance::FaceInFirstLayerSlab(firstForInvalidPool, scene, layerMm, calibBuildDir) &&
            CalibrateDistance::FaceIsLayerCapParallelBuild(firstForInvalidPool, calibBuildDir);

        if (!elephantEdgeSecondPickMode)
        {
            std::unordered_set<const Face *> invalidFaces;
            invalidFaces.reserve(std::min(static_cast<size_t>(256), tris.size() / 2 + 1));
            const CalibWorkflow wFirst =
                scene != nullptr ? CalibrateDistance::ClassifyFace(firstForInvalidPool, scene, layerMm, calibBuildDir,
                                                                   layerHoleInnerEdges)
                                 : CalibWorkflow::Contour;
            for (const PickTriangle &tri : tris)
            {
                const Face *f = tri.face;
                if (f == nullptr)
                    continue;
                if (f == calibFacePoint1 || f == calibFacePoint2)
                    continue;
                if (!CalibrateDistance::FaceNormalPerpendicularToBuild(f, calibBuildDir))
                {
                    invalidFaces.insert(f);
                    continue;
                }
                if (!CalibrateNominal::NormalsAlignedForCalibPick(firstForInvalidPool, f))
                {
                    invalidFaces.insert(f);
                    continue;
                }
                if (scene != nullptr)
                {
                    const CalibWorkflow wf =
                        CalibrateDistance::ClassifyFace(f, scene, layerMm, calibBuildDir, layerHoleInnerEdges);
                    if (!CalibrateDistance::CalibSecondPickWorkflowsCompatible(wFirst, wf))
                        invalidFaces.insert(f);
                }
            }
            if (!invalidFaces.empty())
            {
                // Pool tint must sit near **lit** patch luminance: SRC_ALPHA blend is
                // `α*src + (1-α)*dst`. If src is much lighter than dst, smaller α darkens; larger α
                // lightens — unrelated to “see-through”. Anchor to face albedo + typical diffuse.
                const glm::vec3 albedo = Color::GetFace();
                constexpr float kTypicalDiffuse = 0.55f;
                const glm::vec3 approxLit =
                    glm::min(albedo * (1.0f + SceneLighting::SceneMeshBrightenAmount() * kTypicalDiffuse),
                             glm::vec3(1.0f));
                const glm::vec3 coolShift = glm::vec3(approxLit.r * 0.92f, approxLit.g * 0.96f,
                                                      std::min(1.0f, approxLit.b * 1.1f + 0.02f));
                const glm::vec3 poolTint = glm::mix(approxLit, coolShift, 0.38f);
                uint32_t iv = 0;
                for (const PickTriangle &tri : tris)
                {
                    if (invalidFaces.find(tri.face) == invalidFaces.end())
                        continue;
                    const glm::dvec3 e1 = tri.v1 - tri.v0;
                    const glm::dvec3 e2 = tri.v2 - tri.v0;
                    glm::vec3 n = glm::normalize(glm::vec3(glm::cross(e1, e2)));
                    if (!std::isfinite(static_cast<double>(n.x)) || glm::length(n) < 1e-6f)
                        n = glm::vec3(0.0f, 0.0f, 1.0f);

                    pickHighlightCalibInvalidVertices.push_back({glm::vec3(tri.v0), poolTint, n});
                    pickHighlightCalibInvalidVertices.push_back({glm::vec3(tri.v1), poolTint, n});
                    pickHighlightCalibInvalidVertices.push_back({glm::vec3(tri.v2), poolTint, n});
                    pickHighlightCalibInvalidIndices.push_back(iv);
                    pickHighlightCalibInvalidIndices.push_back(iv + 1);
                    pickHighlightCalibInvalidIndices.push_back(iv + 2);
                    iv += 3;
                }
            }
        }
    }

    {
        const Face *hoverDraw = hoverPickFace;
        if (hoverDraw == calibFacePoint1 || hoverDraw == calibFacePoint2)
            hoverDraw = nullptr;
        // Calibrate edge snap: show edge hover alone — face fill hides whether the hit is edge vs face.
        const bool calibrateEdgeHover =
            activeTool == ActiveTool::Calibrate && hoverPickEdge != nullptr;
        if (hoverDraw != nullptr && !calibrateEdgeHover)
        {
            if (hoverCalibPickRejected)
            {
                const int grayDepth =
                    Color::IsDark() ? RenderingExperiments::kCalibrateRejectHoverGrayUiDepthDark
                                      : RenderingExperiments::kCalibrateRejectHoverGrayUiDepthLight;
                appendFaceTrisSolid(hoverDraw, glm::vec3(Color::GetUI(grayDepth, 1.0f)));
            }
            else
                appendFaceTris(hoverDraw, 0.5f, 0.5f);
        }
    }
    const uint32_t xrayFaceHighlightIndexCount = static_cast<uint32_t>(pickHighlightIndices.size());

    const Edge *hoverEdgeDraw = hoverPickEdge;
    if (hoverEdgeDraw == calibEdgePoint1 || hoverEdgeDraw == calibEdgePoint2)
        hoverEdgeDraw = nullptr;
    if (hoverEdgeDraw != nullptr && activeTool == ActiveTool::Calibrate && hoverCalibPickRejected)
    {
        const int grayDepth =
            Color::IsDark() ? RenderingExperiments::kCalibrateRejectHoverGrayUiDepthDark
                              : RenderingExperiments::kCalibrateRejectHoverGrayUiDepthLight;
        appendEdgeLinesRgb(hoverEdgeDraw, glm::vec3(Color::GetUI(grayDepth, 1.0f)));
    }
    else
        appendEdgeLines(hoverEdgeDraw, 0.5f, 0.5f);
    const uint32_t xrayEdgeHighlightIndexCount = static_cast<uint32_t>(pickHighlightLineIndices.size());

    renderer.UploadPickHighlightMesh(pickHighlightVertices, pickHighlightIndices, xrayFaceHighlightIndexCount);
    renderer.UploadPickHighlightLineMesh(pickHighlightLineVertices, pickHighlightLineIndices,
                                         xrayEdgeHighlightIndexCount);
    renderer.UploadPickHighlightRejectMesh(pickHighlightRejectVertices, pickHighlightRejectIndices);
    renderer.UploadPickHighlightCalibInvalidMesh(pickHighlightCalibInvalidVertices, pickHighlightCalibInvalidIndices);
}

void Display::RefreshCalibSpanOverlayForViewportRender()
{
    calibHoverSpanPreviewActive = false;
    calibHoverSpanLabel.clear();
    calibHoverSpanP0 = glm::dvec3(0.0);
    calibHoverSpanP1 = glm::dvec3(0.0);

    std::vector<Vertex> calibHoverSpanVerts;
    std::vector<uint32_t> calibHoverSpanIdx;

    const auto upload = [&]() { renderer.UploadCalibHoverSpanLineMesh(calibHoverSpanVerts, calibHoverSpanIdx); };

    if (activeTool != ActiveTool::Calibrate || !calibPara_Point1 || !calibPara_Point1->visible || scene == nullptr ||
        (scene->solids.empty() && scene->faces.empty()))
    {
        upload();
        return;
    }

    const glm::vec3 rgb = glm::vec3(Color::GetAccentSteps(0.75f, 1.0f, 0.55f));
    const glm::vec3 lineNormal(0.0f, 0.0f, 1.0f);

    const Face *f1 = ResolveCalibFaceForWorkflow(calibFacePoint1, calibEdgePoint1);
    const Face *f2 = ResolveCalibFaceForWorkflow(calibFacePoint2, calibEdgePoint2);

    const bool hasBoth = CalibSlotHasPick(calibFacePoint1, calibEdgePoint1) &&
                         CalibSlotHasPick(calibFacePoint2, calibEdgePoint2);

    if (hasBoth && f1 != nullptr && f2 != nullptr)
    {
        const bool elephantEdges = calibFacePoint1 == calibFacePoint2 && calibEdgePoint1 != nullptr &&
                                   calibEdgePoint2 != nullptr &&
                                   CalibrateNominal::EdgesAreParallelForCalib(calibEdgePoint1, calibEdgePoint2);
        const CalibrateNominal::SpanPreview sp =
            elephantEdges ? CalibrateNominal::SpanPreviewBetweenParallelEdgesOnFace(f1, calibEdgePoint1,
                                                                                   calibEdgePoint2)
                          : CalibrateNominal::SpanPreviewBetweenFaces(f1, f2);
        if (!sp.valid)
        {
            upload();
            return;
        }
        calibHoverSpanPreviewActive = true;
        calibHoverSpanLabel = FormatCalibSpanMmLabel(sp.nominalMm);
        calibHoverSpanP0 = sp.p0;
        calibHoverSpanP1 = sp.p1;
        calibHoverSpanVerts.push_back({glm::vec3(sp.p0), rgb, lineNormal});
        calibHoverSpanVerts.push_back({glm::vec3(sp.p1), rgb, lineNormal});
        calibHoverSpanIdx.push_back(0);
        calibHoverSpanIdx.push_back(1);
        upload();
        return;
    }

    const bool awaitingSecond =
        calibPara_Point2 && calibPara_Point2->selected && CalibSlotHasPick(calibFacePoint1, calibEdgePoint1);

    if (!awaitingSecond || f1 == nullptr)
    {
        upload();
        return;
    }

    const glm::dvec3 calibBuildDirAwait = CalibrateDistance::DefaultCalibrateBuildDirection();
    const double layerMmAwait = static_cast<double>(layerHeight);

    if (hoverPickFace != nullptr && calibEdgePoint1 != nullptr && hoverPickEdge != nullptr &&
        calibFacePoint1 != nullptr && hoverPickFace == calibFacePoint1 &&
        CalibrateDistance::FaceInFirstLayerSlab(calibFacePoint1, scene, layerMmAwait, calibBuildDirAwait) &&
        CalibrateDistance::FaceIsLayerCapParallelBuild(calibFacePoint1, calibBuildDirAwait))
    {
        const CalibrateNominal::SpanPreview sp =
            CalibrateNominal::SpanPreviewBetweenParallelEdgesOnFace(f1, calibEdgePoint1, hoverPickEdge);
        if (sp.valid)
        {
            calibHoverSpanPreviewActive = true;
            calibHoverSpanLabel = FormatCalibSpanMmLabel(sp.nominalMm);
            calibHoverSpanP0 = sp.p0;
            calibHoverSpanP1 = sp.p1;
            calibHoverSpanVerts.push_back({glm::vec3(sp.p0), rgb, lineNormal});
            calibHoverSpanVerts.push_back({glm::vec3(sp.p1), rgb, lineNormal});
            calibHoverSpanIdx.push_back(0);
            calibHoverSpanIdx.push_back(1);
            upload();
            return;
        }
    }

    if (hoverPickFace != nullptr)
    {
        const CalibrateNominal::SpanPreview sp = CalibrateNominal::SpanPreviewBetweenFaces(f1, hoverPickFace);
        if (sp.valid)
        {
            calibHoverSpanPreviewActive = true;
            calibHoverSpanLabel = FormatCalibSpanMmLabel(sp.nominalMm);
            calibHoverSpanP0 = sp.p0;
            calibHoverSpanP1 = sp.p1;
            calibHoverSpanVerts.push_back({glm::vec3(sp.p0), rgb, lineNormal});
            calibHoverSpanVerts.push_back({glm::vec3(sp.p1), rgb, lineNormal});
            calibHoverSpanIdx.push_back(0);
            calibHoverSpanIdx.push_back(1);
            upload();
            return;
        }
    }

    float mx = 0.0f;
    float my = 0.0f;
    SDL_GetMouseState(&mx, &my);
    int w = 0;
    int h = 0;
    SDL_GetWindowSize(window, &w, &h);
    glm::dvec3 ro;
    glm::dvec3 rd;
    ScenePick::OrthoPickRay(camera, w, h, mx, my, ro, rd);
    const glm::dvec3 centroid = CalibrateNominal::FaceCentroidWorld(f1);
    const double rdLen = glm::length(rd);
    if (rdLen < 1e-30)
    {
        upload();
        return;
    }
    const glm::dvec3 rdUnit = rd / rdLen;
    const double t = -glm::dot(ro - centroid, rdUnit) / rdLen;
    const glm::dvec3 hit = ro + rd * t;

    calibHoverSpanP0 = centroid;
    calibHoverSpanP1 = hit;
    calibHoverSpanVerts.push_back({glm::vec3(centroid), rgb, lineNormal});
    calibHoverSpanVerts.push_back({glm::vec3(hit), rgb, lineNormal});
    calibHoverSpanIdx.push_back(0);
    calibHoverSpanIdx.push_back(1);
    upload();
}

Display::CalibPickHit Display::PickCalibrateAtPixel(float pixelX, float pixelY) const
{
    CalibPickHit out;
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    glm::dvec3 ro, rd;
    ScenePick::OrthoPickRay(camera, w, h, pixelX, pixelY, ro, rd);

    double faceT = 0.0;
    out.face = ScenePick::PickClosestFace(renderer.GetPickTriangles(), ro, rd, PickFilter::Faces, &faceT);
    if (out.face == nullptr || scene == nullptr)
        return out;

    const glm::dvec3 calibBuildDir = CalibrateDistance::DefaultCalibrateBuildDirection();
    const double layerMm = static_cast<double>(layerHeight);
    if (!CalibrateDistance::FaceInFirstLayerSlab(out.face, scene, layerMm, calibBuildDir) ||
        !CalibrateDistance::FaceIsLayerCapParallelBuild(out.face, calibBuildDir))
        return out;

    thread_local std::vector<PickSegment> faceSegScratch;
    faceSegScratch.clear();
    for (const PickSegment &ps : renderer.GetPickSegments())
    {
        if (ps.edge != nullptr && CalibrateNominal::EdgeBelongsToFace(ps.edge, out.face))
            faceSegScratch.push_back(ps);
    }
    if (faceSegScratch.empty())
        return out;

    double rayT = 0.0;
    double distSq = 0.0;
    const Edge *edgeHit = ScenePick::PickClosestEdgeAlongRay(faceSegScratch, ro, rd, kCalibEdgePickMaxDistSqMm,
                                                             &rayT, &distSq);
    if (edgeHit != nullptr)
        out.edge = edgeHit;
    return out;
}

void Display::UpdatePickHover(float pixelX, float pixelY)
{
    ImGuiIO &io = ImGui::GetIO();
    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(nullptr, nullptr);
    const bool viewportNav = (buttons & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) != 0 ||
                             (buttons & SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE)) != 0;

    if (io.WantCaptureMouse || HitTestUI(pixelX, pixelY) || HitTestImGui(pixelX, pixelY) || viewportNav)
    {
        SetHoverCalibPick(nullptr, nullptr);
        return;
    }
    if (GetActivePickFilter() == PickFilter::None)
    {
        SetHoverCalibPick(nullptr, nullptr);
        return;
    }

    const CalibPickHit hit = PickCalibrateAtPixel(pixelX, pixelY);
    const glm::dvec3 calibBuildDir = CalibrateDistance::DefaultCalibrateBuildDirection();
    const double layerMm = static_cast<double>(layerHeight);

    if (calibPara_Point1 && calibPara_Point1->selected &&
        !CalibSlotHasPick(calibFacePoint1, calibEdgePoint1))
    {
        if (hit.face != nullptr &&
            !CalibFacePickPassesWallGate(hit.face, hit.edge, scene, layerMm, calibBuildDir))
        {
            SetHoverCalibPick(hit.face, hit.edge, true);
            return;
        }
    }

    if (calibPara_Point2 && calibPara_Point2->selected && CalibSlotHasPick(calibFacePoint1, calibEdgePoint1))
    {
        const Face *firstResolved = ResolveCalibFaceForWorkflow(calibFacePoint1, calibEdgePoint1);
        std::unordered_set<const Edge *> layerHoleInnerEdges;
        GatherCalibLayerHoleInnerEdges(scene, calibBuildDir, layerHoleInnerEdges);
        if (hit.face != nullptr &&
            !CalibSecondPickAcceptsHit(calibFacePoint1, calibEdgePoint1, firstResolved, hit.face, hit.edge,
                                       scene, layerMm, calibBuildDir, layerHoleInnerEdges))
        {
            SetHoverCalibPick(hit.face, hit.edge, true);
            return;
        }
    }
    SetHoverCalibPick(hit.face, hit.edge, false);
}

void Display::TryCommitCalibrateFacePick(float pixelX, float pixelY)
{
    if (activeTool != ActiveTool::Calibrate)
        return;
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantCaptureMouse || HitTestUI(pixelX, pixelY) || HitTestImGui(pixelX, pixelY))
        return;
    if (!calibPara_Point1 || !calibPara_Point1->visible)
        return;

    const CalibPickHit hit = PickCalibrateAtPixel(pixelX, pixelY);
    if (hit.face == nullptr)
        return;

    const glm::dvec3 calibBuildDirCommit = CalibrateDistance::DefaultCalibrateBuildDirection();
    const double layerMmCommit = static_cast<double>(layerHeight);
    if (!CalibFacePickPassesWallGate(hit.face, hit.edge, scene, layerMmCommit, calibBuildDirCommit))
        return;

    if (calibPara_Point1->selected)
    {
        calibFacePoint1 = hit.face;
        calibEdgePoint1 = hit.edge;
        calibStepPoint1 = Icons::StepState::Done;
        calibPara_Point1->selected = false;
        if (calibPara_Point2)
            calibPara_Point2->selected = !CalibSlotHasPick(calibFacePoint2, calibEdgePoint2);
    }
    else if (calibPara_Point2 && calibPara_Point2->selected)
    {
        const Face *firstResolved = ResolveCalibFaceForWorkflow(calibFacePoint1, calibEdgePoint1);
        const glm::dvec3 calibBuildDir = CalibrateDistance::DefaultCalibrateBuildDirection();
        std::unordered_set<const Edge *> layerHoleInnerEdges;
        GatherCalibLayerHoleInnerEdges(scene, calibBuildDir, layerHoleInnerEdges);
        const double layerMm = static_cast<double>(layerHeight);
        if (!CalibSecondPickAcceptsHit(calibFacePoint1, calibEdgePoint1, firstResolved, hit.face, hit.edge, scene,
                                       layerMm, calibBuildDir, layerHoleInnerEdges))
            return;
        calibFacePoint2 = hit.face;
        calibEdgePoint2 = hit.edge;
        calibStepPoint2 = Icons::StepState::Done;
        calibPara_Point2->selected = false;
        calibPara_Point1->selected = !CalibSlotHasPick(calibFacePoint1, calibEdgePoint1);
    }
    else
        return;

    RefreshCalibWorkflow();
    uiRenderer.MarkDirty();
    MarkPickDirty();
}

void Display::RefreshCalibWorkflow()
{
    if (!scene)
    {
        calibWorkflow = CalibWorkflow::None;
        RefreshCalibCompensation();
        RefreshCalibDerivedRowVisible();
        return;
    }
    const glm::dvec3 calibBuildDir = CalibrateDistance::DefaultCalibrateBuildDirection();
    std::unordered_set<const Edge *> layerHoleInnerEdges;
    GatherCalibLayerHoleInnerEdges(scene, calibBuildDir, layerHoleInnerEdges);
    const double layerMm = static_cast<double>(layerHeight);
    const Face *f1 = ResolveCalibFaceForWorkflow(calibFacePoint1, calibEdgePoint1);
    const Face *f2 = ResolveCalibFaceForWorkflow(calibFacePoint2, calibEdgePoint2);

    const bool elephantFootEdges =
        calibFacePoint1 != nullptr && calibFacePoint1 == calibFacePoint2 && calibEdgePoint1 != nullptr &&
        calibEdgePoint2 != nullptr && calibEdgePoint1 != calibEdgePoint2 &&
        CalibrateDistance::FaceInFirstLayerSlab(calibFacePoint1, scene, layerMm, calibBuildDir) &&
        CalibrateDistance::FaceIsLayerCapParallelBuild(calibFacePoint1, calibBuildDir) &&
        CalibrateNominal::EdgeBelongsToFace(calibEdgePoint1, calibFacePoint1) &&
        CalibrateNominal::EdgeBelongsToFace(calibEdgePoint2, calibFacePoint1) &&
        CalibrateNominal::EdgesAreParallelForCalib(calibEdgePoint1, calibEdgePoint2);

    if (elephantFootEdges)
        calibWorkflow = CalibWorkflow::ElephantFoot;
    else if (f1 != nullptr && f2 != nullptr)
    {
        calibWorkflow =
            CalibrateDistance::CombinePickedFaces(f1, f2, scene, layerMm, calibBuildDir, layerHoleInnerEdges);
        if (calibWorkflow == CalibWorkflow::Contour || calibWorkflow == CalibWorkflow::Hole)
        {
            if (!CalibrateNominal::NominalSpanPerpendicularToBuild(f1, f2, calibBuildDir))
                calibWorkflow = CalibWorkflow::None;
        }
    }
    else if (f1 != nullptr)
        calibWorkflow =
            CalibrateDistance::ClassifyFace(f1, scene, layerMm, calibBuildDir, layerHoleInnerEdges);
    else if (f2 != nullptr)
        calibWorkflow =
            CalibrateDistance::ClassifyFace(f2, scene, layerMm, calibBuildDir, layerHoleInnerEdges);
    else
        calibWorkflow = CalibWorkflow::None;

    RefreshCalibCompensation();
    RefreshCalibDerivedRowVisible();
}

void Display::RefreshCalibDerivedRowVisible()
{
    if (!calibPara_Derived)
        return;

    bool next = false;
    const bool parameterRowsVisible = calibSec_Parameters ? calibSec_Parameters->visible
                                                          : (calibPara_Measure && calibPara_Measure->visible);
    if (parameterRowsVisible)
    {
        const bool importDone = calibStepImport == Icons::StepState::Done;
        const bool hasTwoPicks = CalibSlotHasPick(calibFacePoint1, calibEdgePoint1) &&
                                 CalibSlotHasPick(calibFacePoint2, calibEdgePoint2);
        next = importDone && hasTwoPicks;
    }

    bool changedVis = false;
    if (calibPara_Derived->visible != next)
    {
        calibPara_Derived->visible = next;
        changedVis = true;
    }
    if (calibSec_Result && calibSec_Result->visible != next)
    {
        calibSec_Result->visible = next;
        changedVis = true;
    }
    if (changedVis)
        uiRenderer.MarkDirty();
}

void Display::RefreshCalibCompensation()
{
    calibContourScale = 1.0f;
    calibHoleOffsetMm = 0.0f;
    calibElephantFootMm = 0.0f;
    calibCompensationValid = false;
    calibNominal = 0.0f;

    const Face *spanA = ResolveCalibFaceForWorkflow(calibFacePoint1, calibEdgePoint1);
    const Face *spanB = ResolveCalibFaceForWorkflow(calibFacePoint2, calibEdgePoint2);
    if (scene == nullptr || spanA == nullptr || spanB == nullptr)
        return;

    CalibrateNominal::SpanResult span;
    if (calibWorkflow == CalibWorkflow::ElephantFoot && calibEdgePoint1 != nullptr &&
        calibEdgePoint2 != nullptr && spanA != nullptr && spanA == spanB)
        span = CalibrateNominal::SpanBetweenParallelEdgesOnFace(spanA, calibEdgePoint1, calibEdgePoint2);
    else
        span = CalibrateNominal::SpanBetweenFaces(spanA, spanB);
    if (!span.valid)
        return;

    if (calibWorkflow != CalibWorkflow::ElephantFoot &&
        !CalibrateNominal::NominalSpanPerpendicularToBuild(spanA, spanB, CalibrateDistance::DefaultCalibrateBuildDirection()))
        return;

    calibNominal = span.nominalMm;
    if (calibWorkflow == CalibWorkflow::None)
        return;

    const CalibrateCompensation::Values vals =
        CalibrateCompensation::Compute(calibWorkflow, calibNominal, calibMeasured);
    if (!vals.valid)
        return;

    calibContourScale = vals.contourScale;
    calibHoleOffsetMm = vals.holeRadiusOffsetMm;
    calibElephantFootMm = vals.elephantFootExcessMm;
    calibCompensationValid = true;
}

void Display::snapInput(float &x, float &y)
{
    if (std::hypot(x, y) < kPanSnapTravelFloor)
        return;
    if (std::abs(x) <= std::abs(y) * 0.5f)
        x = 0;
    else if (std::abs(y) <= std::abs(x) * 0.5f)
        y = 0;
}

void Display::BeginOrbitSnapGesture()
{
    orbitSnapGestureActive = true;
    orbitSnapSuppressedAxis = camera.PrincipalSnapAxis(UserTuning::snapEnterDeg);
}

void Display::Orbit(float offsetX, float offsetY)
{
    camera.Orbit(offsetX, offsetY);
    if (orbitSnapGestureActive)
    {
        const float liveSnapDeg = std::max(UserTuning::snapEnterDeg * 2.2f, UserTuning::snapEnterDeg + 2.0f);
        const float releaseDeg = liveSnapDeg + 1.5f;
        if (orbitSnapSuppressedAxis != Camera::OrbitSnapAxis::None &&
            !camera.IsWithinSnapAxis(orbitSnapSuppressedAxis, releaseDeg))
        {
            orbitSnapSuppressedAxis = Camera::OrbitSnapAxis::None;
        }
        camera.SnapToPrincipalAxis(liveSnapDeg, orbitSnapSuppressedAxis);
    }

    UpdateCamera();
}

void Display::FinishOrbitSnap()
{
    camera.FinishOrbitSnap(orbitSnapSuppressedAxis);
    orbitSnapGestureActive = false;
    orbitSnapSuppressedAxis = Camera::OrbitSnapAxis::None;
    UpdateCamera();
}

void Display::Roll(float delta)
{
    camera.Roll(delta);
    UpdateCamera();
}

void Display::Pan(float offsetX, float offsetY, bool scroll)
{
    snapInput(offsetX, offsetY);
    camera.Pan(offsetX, offsetY, scroll);

    UpdateCamera();
}

void Display::FrameScene()
{
    if (scene->points.empty())
        return;

    glm::vec3 min(std::numeric_limits<float>::max());
    glm::vec3 max(std::numeric_limits<float>::lowest());

    for (const auto &point : scene->points)
    {
        glm::vec3 pos(point.position);
        min = glm::min(min, pos);
        max = glm::max(max, pos);
    }

    camera.FrameBounds(min, max);
    UpdateCamera();
}

void Display::ResetCameraView()
{
    camera.ResetHomeView();
    UpdateCamera();
}

bool Display::HitTestUI(float pixelX, float pixelY) const
{
    return uiRenderer.HitTest(pixelX, pixelY);
}

bool Display::HitTestImGui(float pixelX, float pixelY) const
{
    ImGuiContext *ctx = ImGui::GetCurrentContext();
    if (ctx == nullptr)
        return false;
    ImGuiWindow *hovered = nullptr;
    ImGuiWindow *hoveredUnderMoving = nullptr;
    ImGui::FindHoveredWindowEx(ImVec2(pixelX, pixelY), false, &hovered, &hoveredUnderMoving);
    return hovered != nullptr;
}

void Display::MarkBug()
{
    auto &sl = SessionLogger::Instance();
    FillSessionReproState(sl.state);
    sl.LogBugMarker();
}

void Display::FillSessionReproState(SessionState &s) const
{
    if (scene != nullptr)
    {
        s.points = scene->points.size();
        s.edges = scene->edges.size();
        s.faces = scene->faces.size();
        s.solids = scene->solids.size();
    }

    s.overhangAngle = overhangAngle;
    s.sharpCornerAngle = sharpCornerAngle;
    s.thinMinWidth = thinMinWidth;
    s.minFeatureSize = minFeatureSize;
    s.layerHeight = layerHeight;

    s.cameraTarget = camera.target;
    s.cameraOrthoSize = camera.orthoSize;
    s.cameraPosition = camera.GetPosition();
    s.cameraDistance = camera.distance;
    s.cameraQuatW = camera.orientation.w;
    s.cameraQuatX = camera.orientation.x;
    s.cameraQuatY = camera.orientation.y;
    s.cameraQuatZ = camera.orientation.z;
    s.cameraNearPlane = camera.nearPlane;
    s.cameraFarPlane = camera.farPlane;

    s.windowLogicalW = static_cast<int>(windowWidth);
    s.windowLogicalH = static_cast<int>(windowHeight);
    if (window != nullptr)
        SDL_GetWindowSizeInPixels(window, &s.windowPixelsW, &s.windowPixelsH);
    else
    {
        s.windowPixelsW = 0;
        s.windowPixelsH = 0;
    }

    if (activeTool == ActiveTool::Calibrate)
        s.activeToolOrdinal = 1u;
    else if (activeTool == ActiveTool::Structure)
        s.activeToolOrdinal = 2u;
    else
        s.activeToolOrdinal = 0u;
    s.viewportAnalysisEnabled = analysisEnabled;
    s.depthExperimentOrdinal = static_cast<uint8_t>(ViewportDepthExperiments::Active());
}

void Display::SyncToolbarToolVisualState()
{
    if (toolbarAnalysisLine && toolbarCalibrateLine && toolbarStructureLine && uiAnalysis && uiCalibrate &&
        uiStructure)
    {
        toolbarAnalysisLine->selected =
            (activeTool == ActiveTool::Analysis && uiAnalysis->visible);
        toolbarCalibrateLine->selected =
            (activeTool == ActiveTool::Calibrate && uiCalibrate->visible);
        toolbarStructureLine->selected =
            (activeTool == ActiveTool::Structure && uiStructure->visible);
    }
    uiRenderer.MarkDirty();
}

void Display::PublishImportProgress(uint64_t generation, const ImportProgress &progress)
{
    if (generation != importProgressGeneration.load(std::memory_order_relaxed))
        return;

    std::lock_guard<std::mutex> lock(importProgressMutex);
    if (generation != importProgressGeneration.load(std::memory_order_relaxed))
        return;

    latestImportProgressPhase = progress.phase;
    latestImportProgress01 = progress.progress01;
    latestImportProgressDirty = true;
}

void Display::ApplyImportProgressSnapshot()
{
    std::string phase;
    float progress01 = -1.0f;
    {
        std::lock_guard<std::mutex> lock(importProgressMutex);
        if (!latestImportProgressDirty)
            return;

        phase = latestImportProgressPhase;
        progress01 = latestImportProgress01;
        latestImportProgressDirty = false;
    }

    SetImportProgress(std::move(phase), progress01);
}

void Display::ClearPendingImportProgressSnapshot()
{
    std::lock_guard<std::mutex> lock(importProgressMutex);
    latestImportProgressPhase.clear();
    latestImportProgress01 = -1.0f;
    latestImportProgressDirty = false;
}

void Display::SetImportProgress(std::string phase, float progress01)
{
    importProgressPhase = std::move(phase);
    importProgress01 =
        (progress01 >= 0.0f && progress01 <= 1.0f) ? std::clamp(progress01, 0.0f, 1.0f) : -1.0f;
    uiRenderer.MarkDirty();
}

void Display::RefreshToolProcessingCards(bool hasModel, bool geometryOrStyleWork, bool ranMainThreadApplyTask)
{
    bool changed = false;
    auto setVisible = [&](Paragraph *p, bool visible)
    {
        if (!p)
            return;
        if (p->visible != visible)
        {
            p->visible = visible;
            changed = true;
        }
    };
    auto setBool = [&](bool &dst, bool value)
    {
        if (dst != value)
        {
            dst = value;
            changed = true;
        }
    };
    auto setFloat = [&](float &dst, float value)
    {
        if (std::fabs(dst - value) > 1e-6f)
        {
            dst = value;
            changed = true;
        }
    };
    auto setText = [&](Paragraph *p, const std::string &text)
    {
        if (!p || p->values.empty())
            return;
        if (p->values[0].text != text)
        {
            p->values[0].text = text;
            changed = true;
        }
    };
    auto ensureLine = [&](Paragraph *p, std::size_t lineIndex) -> SectionLine *
    {
        if (!p)
            return nullptr;
        while (p->values.size() <= lineIndex)
        {
            SectionLine &line = p->values.emplace_back();
            line.fontScale = 0.85f;
            line.textDepth = 1;
            line.onClick = p->onClick;
            changed = true;
        }
        return &p->values[lineIndex];
    };
    auto setLineText = [&](Paragraph *p, std::size_t lineIndex, const std::string &text)
    {
        SectionLine *line = ensureLine(p, lineIndex);
        if (!line)
            return;
        if (line->text != text)
        {
            line->text = text;
            changed = true;
        }
    };
    auto setLineVisible = [&](Paragraph *p, std::size_t lineIndex, bool visible)
    {
        SectionLine *line = ensureLine(p, lineIndex);
        if (!line)
            return;
        if (line->visible != visible)
        {
            line->visible = visible;
            changed = true;
        }
    };
    auto setAccentCounts = [&](Paragraph *p, int64_t num, int64_t den)
    {
        if (!p)
            return;
        if (p->accentProgressNumerator != num || p->accentProgressDenominator != den || p->accentProgress01 >= 0.0f)
        {
            p->accentProgressNumerator = num;
            p->accentProgressDenominator = den;
            p->accentProgress01 = -1.0f;
            changed = true;
        }
    };
    auto clearAccentCounts = [&](Paragraph *p)
    {
        if (!p)
            return;
        if (p->accentProgressNumerator != -1 || p->accentProgressDenominator != -1)
        {
            p->accentProgressNumerator = -1;
            p->accentProgressDenominator = -1;
            changed = true;
        }
    };

    const bool activeHasModel = hasModel && !pendingImportTabActive;
    const bool importActive = importBusy || pendingImportTask.has_value();
    const bool activePendingImport = pendingImportTabActive && importActive;
    const bool hasCommittedAnalysis = lastCommittedAnalysisForRecolor.has_value();
    const bool queueingFirstAnalysis = pendingAnalysisAfterGeometryRebuild && !hasCommittedAnalysis;
    const bool pendingTintThisScene =
        pendingAnalysisTint.has_value() && pendingAnalysisTint->scene == scene;
    const bool analysisRenderingInScene =
        analysisEnabled && activeAnalysisTintForRebuild.has_value() && renderer.FullRebuildInProgress();
    // Hide verdict/counts only while a worker run is in flight or a completed run is still queued for tint apply.
    // Do NOT include queueingFirstAnalysis here: it stays true until geometry rebuild commits recolor, which is
    // the same window as GPU incremental rebuild — hiding panels on it kept counts/verdict off after results exist.
    const bool analysisPipelineWaiting =
        analysisEnabled && (pendingAnalysisTask.has_value() || pendingTintThisScene);
    // Processing card + bar: full pipeline including "waiting to launch" and GPU tint application.
    const bool analysisBusy =
        analysisEnabled &&
        (queueingFirstAnalysis || analysisPipelineWaiting || analysisRenderingInScene);

    if (!analysisBusy)
    {
        ++analysisProcessingIdleStreak;
        if (analysisProcessingIdleStreak >= 3)
            ResetAnalysisPipelineTotals();
    }
    else
    {
        analysisProcessingIdleStreak = 0;

        if (activeHasModel && scene != nullptr)
            RefreshAnalysisPipelineDenominatorFromScene();

        if (analysisRenderingInScene && renderer.FullRebuildInProgress())
            analysisGpuRebuildStepsCache = renderer.IncrementalRebuildStepsDone();

        const char *phaseTitle = "Working on analysis...";
        if (queueingFirstAnalysis)
            phaseTitle = "Queueing analysis...";
        else if (pendingAnalysisTask.has_value())
            phaseTitle = AnalysisWorkerPhaseTitle(analysisWorkerPhaseIdAtomic.load(std::memory_order_relaxed));
        else if (pendingTintThisScene)
            phaseTitle = "Applying analysis...";
        else if (analysisRenderingInScene)
            phaseTitle = "Rendering analysis...";

        uint64_t numerator = analysisWorkerStepsDone.load(std::memory_order_relaxed);
        numerator += analysisTintStepsDone.load(std::memory_order_relaxed);
        if (analysisRenderingInScene)
            numerator += analysisGpuRebuildStepsCache;

        uint64_t denominator = analysisPipelineDenomTotal;
        if (denominator > 0)
            numerator = std::min(numerator, denominator);

        if (uiAnalysisProcessing)
        {
            setText(uiAnalysisProcessing, phaseTitle);
            if (denominator > 0)
                setAccentCounts(uiAnalysisProcessing, static_cast<int64_t>(numerator), static_cast<int64_t>(denominator));
            else
            {
                clearAccentCounts(uiAnalysisProcessing);
                setFloat(uiAnalysisProcessing->accentProgress01, -1.0f);
            }
        }
    }

    if (uiAnalysisProcessing)
    {
        setVisible(uiAnalysisProcessing, analysisBusy && activeHasModel);
        setBool(uiAnalysisProcessing->accentProgressBar, uiAnalysisProcessing->visible);
        if (!uiAnalysisProcessing->visible)
        {
            clearAccentCounts(uiAnalysisProcessing);
            setFloat(uiAnalysisProcessing->accentProgress01, -1.0f);
        }
    }

    // Import prerequisite row: mirror Files-tab import progress on the Analysis "Import a file" step.
    if (uiImportPara)
    {
        setVisible(uiImportPara, activePendingImport || !activeHasModel);
        setBool(uiImportPara->accentProgressBar, activePendingImport);
        const std::string importPhase =
            importProgressPhase.empty() ? std::string("Importing file...") : importProgressPhase;
        setText(uiImportPara, activePendingImport ? ImportPrerequisiteTitle(importProgress01) : "Import a file");
        setLineText(uiImportPara, 1, activePendingImport ? importPhase : "");
        setLineVisible(uiImportPara, 1, activePendingImport);
        if (activePendingImport)
            setFloat(uiImportPara->accentProgress01,
                     importProgress01 >= 0.0f ? importProgress01 : -1.0f);
        else
            setFloat(uiImportPara->accentProgress01, -1.0f);
    }

    if (uiResult)
        setVisible(uiResult, activeHasModel && analysisUiScene == scene && !analysisPipelineWaiting);
    if (uiVerdict)
        setVisible(uiVerdict, activeHasModel && analysisUiScene == scene && !analysisPipelineWaiting);

    // Import progress lives in the Files bar tab; keep Calibrate panel unobstructed during import.
    const bool calibrateBusy =
        activeHasModel && (geometryOrStyleWork || ranMainThreadApplyTask || renderer.FullRebuildInProgress());
    if (uiCalibrateProcessing)
    {
        const bool show = calibrateBusy && uiCalibrate && uiCalibrate->visible;
        setVisible(uiCalibrateProcessing, show);
        setBool(uiCalibrateProcessing->accentProgressBar, show);
        setFloat(uiCalibrateProcessing->accentProgress01, importProgress01 >= 0.0f ? importProgress01 : -1.0f);
        if (show)
        {
            const std::string importPhase =
                importProgressPhase.empty() ? std::string("Importing model...") : importProgressPhase;
            setText(uiCalibrateProcessing,
                    importBusy ? ImportProgressLabel(importPhase, importProgress01) : "Refreshing calibration...");
            if (!importBusy && uiCalibrateProcessing->accentProgress01 < 0.0f)
                setFloat(uiCalibrateProcessing->accentProgress01, 0.75f);
        }
    }

    if (calibSec_Prerequisites)
    {
        if (calibSec_Prerequisites->visible != !calibrateBusy)
        {
            calibSec_Prerequisites->visible = !calibrateBusy;
            changed = true;
        }
    }
    if (calibSec_Parameters)
    {
        const bool nextVisible = !calibrateBusy && calibStepImport == Icons::StepState::Done;
        if (calibSec_Parameters->visible != nextVisible)
        {
            calibSec_Parameters->visible = nextVisible;
            changed = true;
        }
    }
    if (calibPara_Measure)
    {
        const bool nextVisible = !calibrateBusy && calibStepImport == Icons::StepState::Done;
        if (calibPara_Measure->visible != nextVisible)
        {
            calibPara_Measure->visible = nextVisible;
            changed = true;
        }
    }
    RefreshCalibDerivedRowVisible();
    if (changed)
        uiRenderer.MarkDirty();
}

void Display::FlushImportInputEventTail()
{
    SDL_FlushEvents(SDL_EVENT_FINGER_DOWN, SDL_EVENT_FINGER_CANCELED);
    SDL_FlushEvents(SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_WHEEL);
    if (inputForGestureSync != nullptr)
        inputForGestureSync->NotifySdlEventQueueFlushed();
}

void Display::CompleteFileImport(const std::string &path)
{
    // Legacy synchronous path retained as fallback. Normal imports are now handled
    // by ProcessDeferredImportIfAny() using an async worker.
    auto ext = path.substr(path.find_last_of('.') + 1);
    std::string lower;
    for (char c : ext)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    auto importedScene = std::make_unique<Scene>();
    STLImportStats stlSyncStats;
    if (lower == "stl")
        STLImport::Import(path, importedScene.get(), &stlSyncStats);
    else if (lower == "obj")
        OBJImport::Import(path, importedScene.get());
    else if (lower == "3mf")
        ThreeMFImport::Import(path, importedScene.get());

    ownedScenes.push_back(std::move(importedScene));
    activeSceneIndex = ownedScenes.size() - 1;
    scene = ownedScenes.back().get();
    pendingImportTabActive = false;
    analysisUiScene = nullptr;
    lastVerdictWasPass = false;
    flawOverhang = {};
    flawSharp = {};
    flawThin = {};
    flawSmall = {};
    if (uiVerdict)
        uiVerdict->values.clear();
    FrameScene();
    UpdateScene();

    std::string filename = path.substr(path.find_last_of("/\\") + 1);

    {
        auto &sl = SessionLogger::Instance();
        sl.state.lastFilename = filename;
        sl.state.lastFormat = lower;
        sl.state.points = scene->points.size();
        sl.state.edges = scene->edges.size();
        sl.state.faces = scene->faces.size();
        sl.state.solids = scene->solids.size();
        sl.LogFileImport(filename, lower);
        if (lower == "stl" && stlSyncStats.hasMergeDiagnostics)
            sl.LogStlMergeDiagnostics(filename, stlSyncStats);
    }

    openFiles.push_back(filename);
    RebuildFileTabs();

    calibStepImport = Icons::StepState::Done;
    calibPara_Import->visible = false;
    calibPara_Point1->visible = true;
    calibPara_Point2->visible = true;
    if (calibSec_Parameters)
        calibSec_Parameters->visible = true;
    if (calibPara_Measure)
        calibPara_Measure->visible = true;
    ClearCalibrateFacePicks();
    uiRenderer.MarkDirty();
    renderDirty = true;

    // After returning from the native file dialog, macOS can deliver a short tail of synthesized
    // wheel/touch events (trackpad inertia / focus handoff). Flush SDL queue and sync `Input` touch model.
    FlushImportInputEventTail();

    importBusy = false;
    importProgress01 = -1.0f;
    importProgressPhase.clear();
    ClearPendingImportProgressSnapshot();
    pendingImportTabStem.clear();
    pendingImportTabActive = false;
    pendingFileTabsRebuild = true;
}

void Display::ProcessDeferredImportIfAny()
{
    using namespace std::chrono_literals;

    if (pendingImportTask.has_value())
    {
        std::optional<AsyncImportResult> readyResult = pendingImportTask->TryTake();
        if (!readyResult.has_value())
            return;
        AsyncImportResult result = std::move(*readyResult);
        pendingImportTask.reset();

        if (!result.ok || !result.importedScene)
        {
            if (!result.cancelled)
                LOG_WARN("Import failed for", result.path);
            importBusy = false;
            importProgress01 = -1.0f;
            importProgressPhase.clear();
            ClearPendingImportProgressSnapshot();
            pendingImportTabStem.clear();
            pendingImportTabActive = false;
            pendingFileTabsRebuild = true;
            return;
        }

        struct ImportApplyState
        {
            AsyncImportResult result;
            std::string filename;
            size_t importedSceneIndex = SIZE_MAX;
            bool activateImportedScene = false;
            double frameSceneMs = 0.0;
            double updateSceneMs = 0.0;
        };

        auto state = std::make_shared<ImportApplyState>();
        state->result = std::move(result);
        state->filename = state->result.path.substr(state->result.path.find_last_of("/\\") + 1);

        mainThreadPipeline.Enqueue("import-attach-scene", [this, state](double) -> bool
                                   {
                                       SetImportProgress("Attaching imported scene...", 0.85f);
                                       ownedScenes.push_back(std::move(state->result.importedScene));
                                       state->importedSceneIndex = ownedScenes.size() - 1;
                                       state->activateImportedScene = pendingImportTabActive;
                                       if (state->activateImportedScene)
                                       {
                                           activeSceneIndex = state->importedSceneIndex;
                                           scene = ownedScenes.back().get();
                                           analysisUiScene = nullptr;
                                           lastVerdictWasPass = false;
                                           flawOverhang = {};
                                           flawSharp = {};
                                           flawThin = {};
                                           flawSmall = {};
                                           if (uiVerdict)
                                               uiVerdict->values.clear();
                                           skipAnalysisForNextGeometryRebuild = true;
                                       }
                                       return true;
                                   });

        mainThreadPipeline.Enqueue("import-frame-scene", [this, state](double) -> bool
                                   {
                                       SetImportProgress("Framing imported scene...", 0.90f);
                                       if (!state->activateImportedScene)
                                           return true;
                                       using Clock = std::chrono::steady_clock;
                                       const Clock::time_point tStart = Clock::now();
                                       FrameScene();
                                       state->frameSceneMs =
                                           std::chrono::duration<double, std::milli>(Clock::now() - tStart).count();
                                       return true;
                                   });

        mainThreadPipeline.Enqueue("import-update-scene", [this, state](double) -> bool
                                   {
                                       SetImportProgress("Refreshing viewport data...", 0.95f);
                                       if (!state->activateImportedScene)
                                           return true;
                                       using Clock = std::chrono::steady_clock;
                                       const Clock::time_point tStart = Clock::now();
                                       UpdateScene();
                                       state->updateSceneMs =
                                           std::chrono::duration<double, std::milli>(Clock::now() - tStart).count();
                                       return true;
                                   });

        mainThreadPipeline.Enqueue("import-finalize-ui", [this, state](double) -> bool
                                   {
                                       SetImportProgress("Finalizing import...", 1.0f);
                                       if (state->result.lower == "stl" && state->result.hasStlStats)
                                       {
                                           const double pipelineMs =
                                               state->result.importerMs + state->frameSceneMs + state->updateSceneMs;
                                           LOG_INFO("Import timing STL parseMs", state->result.stlParseMs,
                                                    "mergeMs", state->result.stlMergeMs,
                                                    "stlTotalMs", state->result.stlTotalMs,
                                                    "importerMs", state->result.importerMs,
                                                    "frameSceneMs", state->frameSceneMs,
                                                    "updateSceneMs", state->updateSceneMs,
                                                    "pipelineMs", pipelineMs,
                                                    "triangles", state->result.stlTriangles,
                                                    "uniquePoints", state->result.stlUniquePoints,
                                                    "faces", state->result.stlFaces);
                                       }

                                       auto &sl = SessionLogger::Instance();
                                       Scene *importedScene = state->importedSceneIndex < ownedScenes.size()
                                                                  ? ownedScenes[state->importedSceneIndex].get()
                                                                  : scene;
                                       sl.state.lastFilename = state->filename;
                                       sl.state.lastFormat = state->result.lower;
                                       sl.state.points = importedScene ? importedScene->points.size() : 0;
                                       sl.state.edges = importedScene ? importedScene->edges.size() : 0;
                                       sl.state.faces = importedScene ? importedScene->faces.size() : 0;
                                       sl.state.solids = importedScene ? importedScene->solids.size() : 0;
                                       sl.LogFileImport(state->filename, state->result.lower);

                                       if (state->result.lower == "stl" && state->result.hasStlMergeDiagnostics)
                                       {
                                           STLImportStats stlReplay;
                                           stlReplay.isBinary = state->result.stlIsBinary;
                                           stlReplay.triangleCount = state->result.stlTriangles;
                                           stlReplay.uniquePoints = state->result.stlUniquePoints;
                                           stlReplay.faces = state->result.stlFaces;
                                           stlReplay.parseMs = state->result.stlParseMs;
                                           stlReplay.mergeMs = state->result.stlMergeMs;
                                           stlReplay.totalMs = state->result.stlTotalMs;
                                           stlReplay.hasMergeDiagnostics = true;
                                           stlReplay.mergeDiagnostics = state->result.stlMergeDiagnostics;
                                           sl.LogStlMergeDiagnostics(state->filename, stlReplay);
                                       }

                                       pendingImportTabStem.clear();
                                       pendingImportTabActive = false;
                                       openFiles.push_back(state->filename);
                                       RebuildFileTabs();

                                       if (state->activateImportedScene)
                                       {
                                           calibStepImport = Icons::StepState::Done;
                                           calibPara_Import->visible = false;
                                           calibPara_Point1->visible = true;
                                           calibPara_Point2->visible = true;
                                           if (calibSec_Parameters)
                                               calibSec_Parameters->visible = true;
                                           if (calibPara_Measure)
                                               calibPara_Measure->visible = true;
                                           ClearCalibrateFacePicks();
                                       }
                                       uiRenderer.MarkDirty();
                                       renderDirty = true;

                                       FlushImportInputEventTail();

                                       importBusy = false;
                                       importProgress01 = -1.0f;
                                       importProgressPhase.clear();
                                       ClearPendingImportProgressSnapshot();
                                       return true;
                                   });
        return;
    }

    if (!deferredImportPath)
        return;
    std::string path = std::move(*deferredImportPath);
    deferredImportPath.reset();

    const std::string fname = std::filesystem::path(path).filename().string();
    pendingImportTabStem = std::filesystem::path(path).stem().string();
    pendingImportTabActive = true;
    importBusy = true;
    ClearPendingImportProgressSnapshot();
    const uint64_t importGeneration = importProgressGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    SetImportProgress("Opening file...", 0.0f);
    pendingFileTabsRebuild = true;
    Render();

    mainThreadPipeline.Clear();

    if (pendingImportTask.has_value())
    {
        pendingImportTask->RequestCancel();
        pendingImportTask.reset();
    }

    pendingImportTask = taskRunner.Submit([this, path, importGeneration](const TaskRunner::CancellationToken &token) -> AsyncImportResult
                                          {
                                              using Clock = std::chrono::steady_clock;
                                              AsyncImportResult result;
                                              result.path = path;
                                              ImportProgressCallback importProgress = [this, importGeneration](const ImportProgress &progress)
                                              {
                                                  PublishImportProgress(importGeneration, progress);
                                              };

                                              auto ext = path.substr(path.find_last_of('.') + 1);
                                              for (char c : ext)
                                                  result.lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                                              if (token.IsCancellationRequested())
                                              {
                                                  result.cancelled = true;
                                                  return result;
                                              }

                                              result.importedScene = std::make_unique<Scene>();
                                              const Clock::time_point tImporterStart = Clock::now();

                                              if (result.lower == "stl")
                                              {
                                                  STLImportStats stlStats;
                                                  result.ok = STLImport::Import(path, result.importedScene.get(), &stlStats, &importProgress);
                                                  result.hasStlStats = result.ok;
                                                  if (result.hasStlStats)
                                                  {
                                                      result.stlParseMs = stlStats.parseMs;
                                                      result.stlMergeMs = stlStats.mergeMs;
                                                      result.stlTotalMs = stlStats.totalMs;
                                                      result.stlTriangles = stlStats.triangleCount;
                                                      result.stlUniquePoints = stlStats.uniquePoints;
                                                      result.stlFaces = stlStats.faces;
                                                      result.stlIsBinary = stlStats.isBinary;
                                                      result.hasStlMergeDiagnostics = stlStats.hasMergeDiagnostics;
                                                      result.stlMergeDiagnostics = stlStats.mergeDiagnostics;
                                                  }
                                              }
                                              else if (result.lower == "obj")
                                              {
                                                  result.ok = OBJImport::Import(path, result.importedScene.get(), &importProgress);
                                              }
                                              else if (result.lower == "3mf")
                                              {
                                                  result.ok = ThreeMFImport::Import(path, result.importedScene.get(), &importProgress);
                                              }
                                              else
                                              {
                                                  ReportImportProgress(&importProgress, "Unsupported import format.", 1.0f);
                                                  result.ok = false;
                                              }

                                              if (token.IsCancellationRequested())
                                              {
                                                  result.cancelled = true;
                                                  result.ok = false;
                                              }

                                              result.importerMs = std::chrono::duration<double, std::milli>(Clock::now() - tImporterStart).count();
                                              if (!result.ok)
                                                  result.importedScene.reset();
                                              return result;
                                          });
}

void Display::DoFileImport()
{
    FileImport::OpenFileDialog(window, [this](const std::string &path)
                               {
                                   deferredImportPath = path;
                                   renderDirty = true;
                               });
}

void Display::RebuildFileTabs()
{
    // Compact tab style: natural layer-2 paragraph defaults (margin=INSET, padding=0) — matches Analysis children.

    const bool showPendingImportTab =
        !pendingImportTabStem.empty() && (importBusy || pendingImportTask.has_value());

    uiFiles->children.clear();
    uiFiles->children.reserve(openFiles.size() + 1 + (showPendingImportTab ? 1 : 0)); // tabs + optional import + "+"

    for (size_t i = 0; i < openFiles.size(); i++)
    {
        // Use "file_N" as the paragraph id — unique even if two files share the same name.
        // Visible label comes from line.text, not the id.
        Paragraph &tab = uiFiles->AddParagraph("file_" + std::to_string(i));
        tab.values.reserve(1);
        SectionLine &line = tab.values.emplace_back();
        line.text = std::filesystem::path(openFiles[i]).stem().string();
        line.selected = (!pendingImportTabActive && i == activeSceneIndex);
        line.onClick = [this, i]()
        {
            ClearPickHover();
            ClearCalibrateFacePicks();

            Scene *selectedScene = ownedScenes[i].get();
            const bool sceneChanged = (scene != selectedScene || activeSceneIndex != i);
            scene = selectedScene;
            activeSceneIndex = i;
            pendingImportTabActive = false;
            if (sceneChanged)
            {
                UpdateScene();
                FrameScene();
            }
            pendingFileTabsRebuild = true;
            uiRenderer.MarkDirty();
        };
    }

    if (showPendingImportTab)
    {
        Paragraph &impTab = uiFiles->AddParagraph("file_pending_import");
        impTab.selected = pendingImportTabActive;
        impTab.onClick = [this]()
        {
            if (!pendingImportTabStem.empty() && (importBusy || pendingImportTask.has_value()))
            {
                pendingImportTabActive = true;
                pendingFileTabsRebuild = true;
                uiRenderer.MarkDirty();
                renderDirty = true;
            }
        };
        impTab.values.reserve(1);
        SectionLine &impLine = impTab.values.emplace_back();
        impLine.text.clear();
        impLine.bold = false;
        impLine.textDepth = 2;
        impLine.getMinContentWidthPx = [this]()
        {
            ImFont *f = uiRenderer.GetPixelImFont();
            if (!f)
                return 160.0f;
            const std::string preview = std::string("Importing ") + pendingImportTabStem;
            return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, preview.c_str()).x + ImGui::GetStyle().FramePadding.x * 4.0f;
        };
        impLine.imguiContent = [this](float w, float h, float /*iconOffset*/)
        {
            if (pendingImportTabStem.empty())
                return;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 o = ImGui::GetCursorScreenPos();
            const float pad = ImGui::GetStyle().FramePadding.x;
            const std::string phase =
                importProgressPhase.empty() ? std::string("Importing file...") : importProgressPhase;
            const std::string title = std::string("Importing ") + pendingImportTabStem + " - " +
                                      ImportProgressLabel(phase, importProgress01);
            glm::vec4 tc = Color::GetUIText(2);
            ImFont *font = uiRenderer.GetPixelImFont() ? uiRenderer.GetPixelImFont() : ImGui::GetFont();
            const float fs = font->FontSize;
            const float textX = o.x + pad;
            dl->AddText(font, fs, ImVec2(textX, o.y + pad * 0.5f),
                        ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a)), title.c_str());

            constexpr float barH = 3.0f;
            const float barY = o.y + h - barH - pad * 0.75f;
            const float bx0 = textX;
            const float titleW = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, title.c_str()).x;
            const float bx1 = std::min(textX + titleW, o.x + w - pad);
            if (bx1 <= bx0 + 2.0f)
                return;
            const float rr = barH * 0.5f;
            glm::vec4 trackCol = Color::GetUIText(1);
            trackCol.a *= 0.12f;
            dl->AddRectFilled(ImVec2(bx0, barY), ImVec2(bx1, barY + barH),
                              ImGui::GetColorU32(ImVec4(trackCol.r, trackCol.g, trackCol.b, trackCol.a)), rr);
            glm::vec4 fillCol = Color::GetAccent(2, 1.0f, 1.0f);
            float t = importProgress01;
            if (t < 0.0f)
                t = std::fmod(static_cast<float>(ImGui::GetTime()) * 0.55f, 1.0f);
            t = std::clamp(t, 0.0f, 1.0f);
            const float fillX = bx0 + (bx1 - bx0) * t;
            dl->AddRectFilled(ImVec2(bx0, barY), ImVec2(fillX, barY + barH),
                              ImGui::GetColorU32(ImVec4(fillCol.r, fillCol.g, fillCol.b, fillCol.a)), rr);
        };
    }

    // "+" import button — always at the end
    Paragraph &importTab = uiFiles->AddParagraph("+");
    importTab.values.reserve(1);
    SectionLine &importLine = importTab.values.emplace_back();
    importLine.iconDraw = Icons::ImportFile();
    importLine.onClick = [this]()
    { DoFileImport(); };

    uiRenderer.MarkDirty();
}

void Display::InitUI()
{
    float toolbarWidth = 2.0f;
    float sidebarWidth = 10.0f;

    // ── Settings panel (left column: persistent app settings) ───────────────
    // Sections (Appearance, Viewport, Navigation) are populated further below.
    {
        RootPanel settingsDef;
        settingsDef.id = "Settings";
        settingsDef.bgParentDepth = 0;
        settingsDef.leftAnchor = PanelAnchor{nullptr, PanelAnchor::Left};
        settingsDef.topAnchor = PanelAnchor{nullptr, PanelAnchor::Top};
        settingsDef.bottomAnchor = PanelAnchor{nullptr, PanelAnchor::Bottom};
        settingsDef.header = Header{"Settings", 1.0f, 2};
        uiSettings = &uiRenderer.AddPanel(settingsDef);
        uiSettings->children.reserve(3); // Appearance, Viewport, Navigation
    }

    // ── Toolbar (column 2: tool selector) ────────────────────────────────────
    {
        RootPanel toolbarDef;
        toolbarDef.id = "Toolbar";
        toolbarDef.bgParentDepth = 0;
        toolbarDef.leftAnchor = PanelAnchor{uiSettings, PanelAnchor::Right};
        toolbarDef.topAnchor = PanelAnchor{nullptr, PanelAnchor::Top};
        toolbarDef.bottomAnchor = PanelAnchor{nullptr, PanelAnchor::Bottom};
        toolbarDef.width = toolbarWidth;
        uiToolbar = &uiRenderer.AddPanel(toolbarDef);
        uiToolbar->children.reserve(3);

        {
            Paragraph &p = uiToolbar->AddParagraph("ToolAnalysis");
            p.values.reserve(1);
            SectionLine &line = p.values.emplace_back();
            line.iconDraw = Icons::ToolAnalysis();
            line.fontScale = 1.4f;
            line.squareIconHit = true;
            line.selected = true; // Analysis is the default active tool
            line.onClick = [this]()
            {
                if (activeTool == ActiveTool::Analysis)
                {
                    uiAnalysis->visible = !uiAnalysis->visible;
                    if (!uiAnalysis->visible)
                    {
                        ClearPickHover();
                        ClearCalibrateFacePicks();
                    }
                    if (analysisEnabled != uiAnalysis->visible)
                    {
                        analysisEnabled = uiAnalysis->visible;
                        UpdateScene();
                    }
                    SyncToolbarToolVisualState();
                    renderDirty = true;
                    RefreshUIMinWindowSize();
                    return;
                }
                activeTool = ActiveTool::Analysis;
                pendingToolSwitch = true;
                renderDirty = true;
            };
            toolbarAnalysisLine = &line;
        }
        {
            Paragraph &p = uiToolbar->AddParagraph("ToolCalibrate");
            p.values.reserve(1);
            SectionLine &line = p.values.emplace_back();
            line.iconDraw = Icons::ToolCalibrate();
            line.fontScale = 1.4f;
            line.squareIconHit = true;
            line.onClick = [this]()
            {
                if (activeTool == ActiveTool::Calibrate)
                {
                    uiCalibrate->visible = !uiCalibrate->visible;
                    ClearPickHover();
                    ClearCalibrateFacePicks();
                    SyncToolbarToolVisualState();
                    renderDirty = true;
                    RefreshUIMinWindowSize();
                    return;
                }
                activeTool = ActiveTool::Calibrate;
                pendingToolSwitch = true;
                renderDirty = true;
            };
            toolbarCalibrateLine = &line;
        }
        {
            Paragraph &p = uiToolbar->AddParagraph("ToolStructure");
            p.values.reserve(1);
            SectionLine &line = p.values.emplace_back();
            line.iconDraw = Icons::ToolStructure();
            line.fontScale = 1.4f;
            line.squareIconHit = true;
            line.onClick = [this]()
            {
                if (activeTool == ActiveTool::Structure)
                {
                    uiStructure->visible = !uiStructure->visible;
                    ClearPickHover();
                    ClearCalibrateFacePicks();
                    RefreshStructurePreviewForRenderer();
                    MarkGeometryDirtyAll();
                    SyncToolbarToolVisualState();
                    renderDirty = true;
                    RefreshUIMinWindowSize();
                    return;
                }
                activeTool = ActiveTool::Structure;
                pendingToolSwitch = true;
                renderDirty = true;
            };
            toolbarStructureLine = &line;
        }
    }

    // Files tab bar — spans from toolbar right edge to screen right
    RootPanel filesDef;
    filesDef.id = "Files";
    filesDef.bgParentDepth = 0;
    filesDef.horizontal = true;
    filesDef.leftAnchor = PanelAnchor{uiToolbar, PanelAnchor::Right};
    filesDef.rightAnchor = PanelAnchor{nullptr, PanelAnchor::Right};
    filesDef.topAnchor = PanelAnchor{nullptr, PanelAnchor::Top};
    filesDef.minWidth = sidebarWidth;
    filesDef.header = Header{"Files", 1.0f, 2};
    uiFiles = &uiRenderer.AddPanel(filesDef);
    RebuildFileTabs();

    // Analysis panel with sections
    RootPanel analysisDef;
    analysisDef.id = "Analysis";
    analysisDef.bgParentDepth = 0;
    analysisDef.leftAnchor = PanelAnchor{uiToolbar, PanelAnchor::Right};
    analysisDef.topAnchor = PanelAnchor{uiFiles, PanelAnchor::Bottom};
    uiAnalysis = &uiRenderer.AddPanel(analysisDef);

#if 1 // DEBUG: panel-only mode — sections/content hidden for layout debugging
    uiAnalysis->header = Header{"Analysis", 1.0f, 2};
    {
        Paragraph &sub = uiAnalysis->subtitle.emplace();
        sub.values.reserve(1);
        SectionLine &line = sub.values.emplace_back();
        line.text = "Detect possible 3D printing issues by analyzing geometry";
        line.textDepth = 1;
    }
    uiAnalysis->children.reserve(5); // stable pointers: Result + ImportAction + Verdict + Configs + Processing
    uiResult = &uiAnalysis->AddParagraph("Result");
    uiResult->visible = false;
    {
        PrerequisiteDef importDef;
        importDef.id          = "ImportAction";
        importDef.title       = "Import a file";
        importDef.leadingDraw = Icons::CheckBox(&analysisStepImport);
        importDef.active      = true;
        importDef.onClick     = [this]() { DoFileImport(); };
        uiAnalysis->AddParagraph(importDef.id) = BuildPrerequisiteParagraph(importDef);
        uiImportPara = &std::get<Paragraph>(uiAnalysis->children.back());
    }
    uiVerdict = &uiAnalysis->AddParagraph("Verdict");
    uiVerdict->visible = false;

    // Merged result+config rows — always present once a model is loaded.
    // Each row shows: [icon] [count label (flaw color)] · [param value (dim)] [unit]
    // DragFloat spans the full row; a left-zone InvisibleButton handles click-to-navigate.
    uiResult = &uiAnalysis->AddParagraph("Result");
    uiResult->visible = false;
    uiResult->values.reserve(4);

    // Helper: builds an imguiContent lambda for a merged flaw+param row.
    // flawResult    = member to read count/frameCallback from (captured by ref via this)
    // flawColor     = bright flaw color for count+label text
    // countLabel    = e.g. " overhang"  (leading space, singular; "s" appended when count>1)
    // paramLabel    = short label shown dim to the right of the value, e.g. "°" or " mm"
    // getValue      = getter lambda returning float
    // setValue      = setter lambda (float)
    // dragSpeed/min/max/fmt = DragFloat parameters

    auto makeFlawRow = [this](
                           SectionLine &line,
                           glm::vec4 flawColor,
                           Icons::DrawFn icon,
                           const char *countLabel, // singular, e.g. " overhang"
                           const char *plural,     // suffix when count>1, e.g. "s"
                           FlawResult Display::*flawMember,
                           float &param,
                           float dragSpeed, float dragMin, float dragMax,
                           const char *unit, // e.g. "°" or "mm"
                           const char *dragId,
                           const char *minWidthLabel, // e.g. "45°" for min width estimate
                           bool storageIsLengthMm, // true: param stored in mm; UI uses default unit + suffix parse
                           bool isAngle             // true = format "%.0f", false = "%.2f"/"%.1f" based on dragSpeed
                       )
    {
        auto lengthDisplay = storageIsLengthMm ? std::make_shared<float>(0.f) : nullptr;
        auto lengthText = storageIsLengthMm ? std::make_shared<std::array<char, 128>>() : nullptr;
        auto lenTextModeFlag = storageIsLengthMm ? std::make_shared<bool>(false) : nullptr;

        line.iconDraw = [this, flawMember, icon](ImDrawList *dl, float x, float midY, float s)
        {
            FlawResult &fr = this->*flawMember;
            if (fr.count == 0)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.35f);
            icon(dl, x, midY, s);
            if (fr.count == 0)
                ImGui::PopStyleVar();
        };

        // Minimum content width (excluding icon slot, which computeParagraphBox adds separately).
        line.getMinContentWidthPx = [this, countLabel, plural, minWidthLabel, unit, isAngle, storageIsLengthMm]() -> float
        {
            ImFont *f = uiRenderer.GetPixelImFont();
            if (!f)
                return 0.0f;
            float pad = ImGui::GetStyle().FramePadding.x;
            std::string longestCount = std::string("No") + countLabel + plural;
            float countW = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, longestCount.c_str()).x;
            std::string longestVal = isAngle ? std::string(minWidthLabel) + unit
                                             : (storageIsLengthMm ? std::string("0000.0000 in")
                                                                  : std::string(minWidthLabel) + " " + unit);
            float valW = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, longestVal.c_str()).x;
            constexpr float gap = 24.0f;
            return pad * 2.0f + countW + gap + valW;
        };

        line.imguiContent = [this, flawColor, countLabel, plural, flawMember,
                             &param, dragSpeed, dragMin, dragMax,
                             unit, dragId, isAngle, storageIsLengthMm, lengthDisplay, lengthText,
                             lenTextModeFlag](float w, float h, float iconOffset)
        {
            FlawResult &fr = this->*flawMember;
            const LengthUnit defaultLen = LengthUnitFromIndex(settings.defaultLengthUnit);
            glm::vec4 dimColor = Color::GetUIText(1);
            glm::vec4 dimLow = Color::GetUIText(0);

            UIStyle::PushInputStyle(h, dimColor);
            float normalPad = ImGui::GetStyle().FramePadding.x;
            ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
            float originX = rowOrigin.x;

            ImFont *rowFont = FontOrInteractiveRow(uiRenderer, nullptr);
            const float rowFs = rowFont ? rowFont->FontSize : ImGui::GetFontSize();

            // ── Left nav zone: InvisibleButton placed BEFORE DragFloat ──────────
            // Compute left zone width from the same string we draw (zero-count uses
            // "No…" + plural, which is wider than "0" + singular — mismatch used to
            // shrink the param zone and let the value hover tint overlap the title).
            char countBuf[64];
            if (fr.count > 0)
                snprintf(countBuf, sizeof(countBuf), "%zu%s%s", fr.count, countLabel,
                         fr.count > 1 ? plural : "");
            else
                snprintf(countBuf, sizeof(countBuf), "No%s%s", countLabel, plural);
            float leftW = iconOffset + (rowFont ? rowFont->CalcTextSizeA(rowFs, FLT_MAX, 0.0f, countBuf).x
                                                     : ImGui::CalcTextSize(countBuf).x) +
                           normalPad * 2.5f;
            leftW = std::min(leftW, w * 0.65f); // never crowd out the param zone

            bool navFired = false;
            bool showEdit = fr.editing || fr.focusPending;

            if (!showEdit)
            {
                char navId[64];
                snprintf(navId, sizeof(navId), "##nav%s", dragId);
                ImGui::InvisibleButton(navId, ImVec2(leftW, h));

                if (ImGui::IsItemActivated())
                {
                    fr.navTracking = true;
                    fr.navStart = ImGui::GetIO().MousePos;
                }
                if (fr.navTracking && !ImGui::IsItemActive())
                {
                    ImVec2 ep = ImGui::GetIO().MousePos;
                    float d = (ep.x - fr.navStart.x) * (ep.x - fr.navStart.x) +
                              (ep.y - fr.navStart.y) * (ep.y - fr.navStart.y);
                    if (d < 9.0f && fr.count > 0 && fr.frameCallback)
                        navFired = true;
                    fr.navTracking = false;
                }
                // Hover tint on left zone (only when there's something to navigate to)
                if (fr.count > 0 && fr.frameCallback)
                    UIStyle::DrawInputHoverTint(1);
                else if (ImGui::IsItemHovered())
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow); // no pointer when non-navigable
            }

            // Always position DragFloat at the right zone — even in edit mode.
            // Add a small gap in edit mode so the input field doesn't butt up against the label.
            float editGap = showEdit ? normalPad * 3.0f : 0.0f;

            // ── Right param zone: DragFloat ──────────────────────────────────────
            float rightW = w - leftW - editGap;
            const float paramLeft = originX + leftW + editGap;
            // Right edge of the value control (inside the same rounded frame as ImGui item).
            const float paramZoneRight = paramLeft + rightW;

            if (fr.requestEdit)
            {
                if (storageIsLengthMm && lenTextModeFlag && lengthText)
                {
                    *lenTextModeFlag = true;
                    std::snprintf(lengthText->data(), lengthText->size(), "%.6g %s",
                                  static_cast<double>(FromMillimeters(param, defaultLen)),
                                  LengthUnitAbbreviation(defaultLen));
                }
                ImGui::SetKeyboardFocusHere();
                fr.requestEdit = false;
                fr.focusPending = true;
                showEdit = true;
            }

            const char *fmt = showEdit ? (isAngle ? "%.0f" : (dragSpeed < 0.1f ? "%.2f" : "%.1f")) : "";
            if (storageIsLengthMm && !isAngle)
                fmt = showEdit ? "%.6g" : "";

            bool changed = false;
            bool committedEdit = false;

            if (storageIsLengthMm && lengthDisplay && lengthText && lenTextModeFlag &&
                *lenTextModeFlag)
            {
                ImGui::SetCursorScreenPos(ImVec2(paramZoneRight - rightW, rowOrigin.y));
                ImGui::SetNextItemWidth(rightW);
                char inputId[72];
                std::snprintf(inputId, sizeof(inputId), "##ltxt%s", dragId);
                bool textChanged = ImGui::InputText(inputId, lengthText->data(), lengthText->size(),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
                (void)textChanged;
                UIStyle::DrawInputHoverTint(1);
                fr.editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
                if (fr.editing)
                    fr.focusPending = false;
                committedEdit = *lenTextModeFlag && ImGui::IsItemDeactivated();
                if (committedEdit)
                {
                    float mm = param;
                    if (TryParseLengthToMm(std::string_view(lengthText->data()), defaultLen, mm))
                    {
                        param = mm;
                        *lengthDisplay = FromMillimeters(param, defaultLen);
                        changed = true;
                    }
                    else
                    {
                        std::snprintf(lengthText->data(), lengthText->size(), "%.6g %s",
                                      static_cast<double>(FromMillimeters(param, defaultLen)),
                                      LengthUnitAbbreviation(defaultLen));
                    }
                    *lenTextModeFlag = false;
                    fr.focusPending = false;
                }
            }
            else if (storageIsLengthMm && lengthDisplay)
            {
                float itemW = rightW;
                if (showEdit && !(lenTextModeFlag && *lenTextModeFlag))
                {
                    const float sw =
                        ImGui::CalcTextSize(LengthUnitAbbreviation(defaultLen)).x +
                        ImGui::GetStyle().ItemInnerSpacing.x * 2.0f;
                    itemW = std::max(rightW - sw, ImGui::GetFontSize() * 2.5f);
                }
                ImGui::SetCursorScreenPos(ImVec2(paramZoneRight - itemW, rowOrigin.y));
                ImGui::SetNextItemWidth(itemW);

                if (!ImGui::IsItemActive())
                    *lengthDisplay = FromMillimeters(param, defaultLen);
                const float mmPer = MillimetersPerUnit(defaultLen);
                const float duSpeed = (mmPer > 0.0f) ? dragSpeed / mmPer : dragSpeed;
                const float dmin = FromMillimeters(dragMin, defaultLen);
                const float dmax = FromMillimeters(dragMax, defaultLen);
                changed = ImGui::DragFloat(dragId, lengthDisplay.get(), duSpeed, dmin, dmax, fmt);
                committedEdit = ImGui::IsItemDeactivatedAfterEdit();
                UIStyle::DrawInputHoverTint(1);
                if (changed)
                    param = ToMillimeters(*lengthDisplay, defaultLen);
                if (committedEdit)
                    param = ToMillimeters(*lengthDisplay, defaultLen);
                if (ImGui::IsItemActivated())
                    *lenTextModeFlag = false;
                fr.editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
                if (fr.editing)
                    fr.focusPending = false;
                if (ImGui::IsItemActivated())
                {
                    fr.tracking = true;
                    fr.startPos = ImGui::GetIO().MousePos;
                }
                if (fr.tracking && !ImGui::IsItemActive())
                {
                    ImVec2 ep = ImGui::GetIO().MousePos;
                    float d = (ep.x - fr.startPos.x) * (ep.x - fr.startPos.x) +
                              (ep.y - fr.startPos.y) * (ep.y - fr.startPos.y);
                    if (d < 9.0f)
                        fr.requestEdit = true;
                    fr.tracking = false;
                }
            }
            else
            {
                ImGui::SetCursorScreenPos(ImVec2(paramLeft, rowOrigin.y));
                ImGui::SetNextItemWidth(rightW);
                changed = ImGui::DragFloat(dragId, &param, dragSpeed, dragMin, dragMax, fmt);
                committedEdit = ImGui::IsItemDeactivatedAfterEdit();
                UIStyle::DrawInputHoverTint(1);

                fr.editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
                if (fr.editing)
                    fr.focusPending = false;

                if (ImGui::IsItemActivated())
                {
                    fr.tracking = true;
                    fr.startPos = ImGui::GetIO().MousePos;
                }
                if (fr.tracking && !ImGui::IsItemActive())
                {
                    ImVec2 ep = ImGui::GetIO().MousePos;
                    float d = (ep.x - fr.startPos.x) * (ep.x - fr.startPos.x) +
                              (ep.y - fr.startPos.y) * (ep.y - fr.startPos.y);
                    if (d < 9.0f)
                        fr.requestEdit = true;
                    fr.tracking = false;
                }
            }

            // showEdit may change after InputText / DragFloat this frame
            showEdit = fr.editing || fr.focusPending;
            if (storageIsLengthMm && lenTextModeFlag && *lenTextModeFlag)
                showEdit = true;

            // ── Text overlay ─────────────────────────────────────────────────────
            float ty = ImGui::GetItemRectMin().y + ImGui::GetStyle().FramePadding.y;
            ImU32 flawCol = ImGui::GetColorU32(ImVec4(flawColor.r, flawColor.g, flawColor.b, flawColor.a));
            ImU32 dimCol = ImGui::GetColorU32(ImVec4(dimLow.r, dimLow.g, dimLow.b, dimLow.a));
            ImU32 dimColZero = ImGui::GetColorU32(ImVec4(dimLow.r, dimLow.g, dimLow.b, dimLow.a * 0.5f));

            // Left label — always visible (even during text edit); countBuf matches layout above.
            if (rowFont)
            {
                if (fr.count > 0)
                    ImGui::GetWindowDrawList()->AddText(rowFont, rowFs, ImVec2(originX + iconOffset + normalPad, ty),
                                                        flawCol, countBuf);
                else
                    ImGui::GetWindowDrawList()->AddText(rowFont, rowFs, ImVec2(originX + iconOffset + normalPad, ty),
                                                        dimColZero, countBuf);
            }
            else
            {
                if (fr.count > 0)
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(originX + iconOffset + normalPad, ty), flawCol, countBuf);
                else
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(originX + iconOffset + normalPad, ty), dimColZero, countBuf);
            }

            // Right side: readout and edit hints — align to param zone right edge (inside ImGui frame)
            const char *editUnitHint =
                (storageIsLengthMm && !isAngle) ? LengthUnitAbbreviation(defaultLen) : unit;
            const bool skipEditUnitOverlay =
                (storageIsLengthMm && !isAngle && lenTextModeFlag && *lenTextModeFlag);
            if (!showEdit)
            {
                char valBuf[48];
                if (isAngle)
                    snprintf(valBuf, sizeof(valBuf), "%.0f%s", param, unit);
                else if (storageIsLengthMm)
                    FormatLengthMmForDisplay(valBuf, sizeof(valBuf), param, defaultLen);
                else if (dragSpeed < 0.1f)
                    snprintf(valBuf, sizeof(valBuf), "%.2f %s", param, unit);
                else
                    snprintf(valBuf, sizeof(valBuf), "%.1f %s", param, unit);
                ImVec2 vs = rowFont ? rowFont->CalcTextSizeA(rowFs, FLT_MAX, 0.0f, valBuf) : ImGui::CalcTextSize(valBuf);
                ImU32 valCol = (fr.count > 0) ? dimCol : dimColZero;
                if (rowFont)
                    ImGui::GetWindowDrawList()->AddText(rowFont, rowFs,
                                                        ImVec2(paramZoneRight - normalPad - vs.x, ty), valCol, valBuf);
                else
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(paramZoneRight - normalPad - vs.x, ty), valCol, valBuf);
            }
            else if (!skipEditUnitOverlay)
            {
                ImVec2 us =
                    rowFont ? rowFont->CalcTextSizeA(rowFs, FLT_MAX, 0.0f, editUnitHint) : ImGui::CalcTextSize(editUnitHint);
                if (rowFont)
                    ImGui::GetWindowDrawList()->AddText(rowFont, rowFs,
                                                        ImVec2(paramZoneRight - normalPad - us.x, ty), dimCol, editUnitHint);
                else
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(paramZoneRight - normalPad - us.x, ty), dimCol, editUnitHint);
            }

            if (navFired)
                fr.frameCallback();
            if (changed || committedEdit)
            {
                auto &sl = SessionLogger::Instance();
                sl.state.overhangAngle = this->overhangAngle;
                sl.state.sharpCornerAngle = this->sharpCornerAngle;
                sl.state.thinMinWidth = this->thinMinWidth;
                sl.state.minFeatureSize = this->minFeatureSize;
                sl.state.layerHeight = this->layerHeight;
                sl.LogParamChange(std::string(dragId + 2), param);
                RebuildAnalysis();
                UpdateScene();
            }
            UIStyle::PopInputStyle();
        };
    };

    glm::vec4 overhangColor = {Color::GetFace(FaceFlawKind::OVERHANG).r + 0.4f,
                               Color::GetFace(FaceFlawKind::OVERHANG).g + 0.2f,
                               Color::GetFace(FaceFlawKind::OVERHANG).b + 0.2f, 1.0f};
    glm::vec4 thinColor = {Color::GetFace(FaceFlawKind::THIN_SECTION).r + 0.4f,
                           Color::GetFace(FaceFlawKind::THIN_SECTION).g + 0.3f,
                           Color::GetFace(FaceFlawKind::THIN_SECTION).b + 0.15f, 1.0f};
    glm::vec4 smallColor = {Color::GetFace(FaceFlawKind::SMALL_FEATURE).r + 0.4f,
                            Color::GetFace(FaceFlawKind::SMALL_FEATURE).g + 0.4f,
                            Color::GetFace(FaceFlawKind::SMALL_FEATURE).b + 0.2f, 1.0f};
    glm::vec4 edgeColor = {Color::GetEdge(EdgeFlawKind::SHARP_CORNER).r + 0.3f,
                           Color::GetEdge(EdgeFlawKind::SHARP_CORNER).g + 0.1f,
                           Color::GetEdge(EdgeFlawKind::SHARP_CORNER).b + 0.1f, 1.0f};
    makeFlawRow(uiResult->values.emplace_back(), overhangColor,
                Icons::Overhang(overhangColor),
                " overhang", "s", &Display::flawOverhang,
                overhangAngle, 0.5f, 0.0f, 90.0f, "\u00b0", "##overhang", "90", false, true);

    makeFlawRow(uiResult->values.emplace_back(), edgeColor,
                Icons::SharpCorner(edgeColor),
                " sharp edge", "s", &Display::flawSharp,
                sharpCornerAngle, 0.5f, 0.0f, 180.0f, "\u00b0", "##sharp", "180", false, true);

    makeFlawRow(uiResult->values.emplace_back(), thinColor,
                Icons::ThinSection(thinColor),
                " thin section", "s", &Display::flawThin,
                thinMinWidth, 0.05f, 0.1f, 50.0f, "mm", "##thinsection", "2.0", true, false);

    makeFlawRow(uiResult->values.emplace_back(), smallColor,
                Icons::SmallFeature(smallColor),
                " small feature", "s", &Display::flawSmall,
                minFeatureSize, 0.05f, 0.1f, 50.0f, "mm", "##smallfeature", "10.0", true, false);

    uiAnalysisProcessing = &uiAnalysis->AddParagraph("Processing");
    uiAnalysisProcessing->visible = false;
    uiAnalysisProcessing->dimFill = true;
    uiAnalysisProcessing->padding = UIGrid::GAP * UIElement::INSET_RATIO * 0.85f;
    uiAnalysisProcessing->values.reserve(1);
    {
        SectionLine &line = uiAnalysisProcessing->values.emplace_back();
        line.text = "Analysing faces...";
        line.textDepth = 2;
    }

    RebuildAnalysis();

#endif // DEBUG: panel-only mode

    // Settings panel — left column; extends from bottom of Analysis to bottom of screen.
    // Covers appearance, viewport, and navigation configuration.
    settingsAccentHue = Color::GetAccentHue();
    settingsAccentSat = Color::GetAccentSat();
    ImFont *settingsBodyFont = uiRenderer.GetBodyImFont();

    // Helper: builds a DragFloat row with a left label and right value overlay.
    auto makeSettingsDrag = [this, settingsBodyFont](
                                SectionLine &line,
                                const char *label,
                                float &param,
                                float speed, float minVal, float maxVal,
                                const char *valueFmt, // complete snprintf format, e.g. "%.0f\u00b0"
                                const char *dragId,
                                std::function<void()> onChange)
    {
        line.getMinContentWidthPx = [this, settingsBodyFont, label]() -> float
        {
            ImFont *f = uiRenderer.GetPixelImFont();
            if (!f)
                f = settingsBodyFont;
            if (!f)
                return 0.0f;
            float pad = ImGui::GetStyle().FramePadding.x;
            float labelW = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, label).x;
            constexpr float minValueAreaW = 40.0f; // room for value text + drag affordance
            constexpr float gap = 24.0f;
            return pad * 2.0f + labelW + gap + minValueAreaW;
        };

        struct DragEditState
        {
            bool tracking = false;
            bool requestEdit = false;
            bool editing = false;
            bool focusPending = false;
            ImVec2 startPos{};
        };
        auto dragState = std::make_shared<DragEditState>();

        line.imguiContent = [this, label, &param, speed, minVal, maxVal,
                             valueFmt, dragId, onChange = std::move(onChange),
                             settingsBodyFont, dragState](float w, float h, float iconOffset)
        {
            glm::vec4 tcLabel = Color::GetUIText(2); // label: prominent
            glm::vec4 tcValue = Color::GetUIText(0); // value: subdued
            float pad = ImGui::GetStyle().FramePadding.x;

            UIStyle::PushInputStyle(h, tcLabel);
            ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
            float originX = rowOrigin.x;

            ImFont *rowFont = FontOrInteractiveRow(uiRenderer, settingsBodyFont);
            const float fs = rowFont ? rowFont->FontSize : ImGui::GetFontSize();

            bool showEdit = dragState->editing || dragState->focusPending;

            if (dragState->requestEdit)
            {
                ImGui::SetKeyboardFocusHere();
                dragState->requestEdit = false;
                dragState->focusPending = true;
                showEdit = true;
            }

            float dragW, dragOffsetX;
            if (showEdit)
            {
                // Edit mode: right zone only so ImGui cursor text doesn't overlap the label.
                float labelTextW =
                    rowFont ? rowFont->CalcTextSizeA(fs, FLT_MAX, 0.0f, label).x : ImGui::CalcTextSize(label).x;
                float leftW = std::min(iconOffset + pad + labelTextW + pad * 2.5f, w * 0.6f);
                dragOffsetX = leftW;
                dragW = w - leftW;
            }
            else
            {
                // Normal mode: full row width — label and value are painted on top.
                dragOffsetX = 0.0f;
                dragW = w;
            }

            ImGui::SetCursorScreenPos(ImVec2(originX + dragOffsetX, rowOrigin.y));
            ImGui::SetNextItemWidth(dragW);
            bool changed = ImGui::DragFloat(dragId, &param, speed, minVal, maxVal,
                                            showEdit ? valueFmt : "");
            UIStyle::DrawInputHoverTint(1);

            dragState->editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
            if (dragState->editing)
                dragState->focusPending = false;

            if (ImGui::IsItemActivated())
            {
                dragState->tracking = true;
                dragState->startPos = ImGui::GetIO().MousePos;
            }
            if (dragState->tracking && !ImGui::IsItemActive())
            {
                ImVec2 ep = ImGui::GetIO().MousePos;
                float dx = ep.x - dragState->startPos.x;
                float dy = ep.y - dragState->startPos.y;
                if (dx * dx + dy * dy < 9.0f)
                    dragState->requestEdit = true;
                dragState->tracking = false;
            }

            ImDrawList *dl = ImGui::GetWindowDrawList();
            float bottom = ImGui::GetItemRectMax().y - ImGui::GetStyle().FramePadding.y;

            // Label: always drawn over the drag widget.
            ImU32 labelCol = ImGui::GetColorU32(ImVec4(tcLabel.r, tcLabel.g, tcLabel.b, tcLabel.a));
            {
                float ty_label = bottom - fs;
                if (rowFont)
                    dl->AddText(rowFont, fs, ImVec2(originX + iconOffset + pad, ty_label), labelCol, label);
                else
                    dl->AddText(ImVec2(originX + iconOffset + pad, ty_label), labelCol, label);
            }

            // Value overlay: right edge, hidden during text edit (DragFloat renders it).
            if (!showEdit)
            {
                char valBuf[32];
                snprintf(valBuf, sizeof(valBuf), valueFmt, param);
                ImVec2 vs = rowFont ? rowFont->CalcTextSizeA(fs, FLT_MAX, 0.0f, valBuf) : ImGui::CalcTextSize(valBuf);
                float ty_value = bottom - fs;
                ImU32 valCol = ImGui::GetColorU32(ImVec4(tcValue.r, tcValue.g, tcValue.b, tcValue.a));
                if (rowFont)
                    dl->AddText(rowFont, fs, ImVec2(originX + w - pad - vs.x, ty_value), valCol, valBuf);
                else
                    dl->AddText(ImVec2(originX + w - pad - vs.x, ty_value), valCol, valBuf);
            }

            if (changed)
                onChange();
            UIStyle::PopInputStyle();
        };
    };

    // Same row layout as makeSettingsDrag, but `paramMm` is stored in millimeters; drag/parse uses
    // settings.defaultLengthUnit, with optional mm/cm/in/ft suffix when editing as text.
    auto makeSettingsLengthDrag = [this, settingsBodyFont](
                                      SectionLine &line,
                                      const char *label,
                                      float &paramMm,
                                      float speedMm, float minMm, float maxMm,
                                      const char *dragId,
                                      std::function<void()> onChange)
    {
        line.getMinContentWidthPx = [this, settingsBodyFont, label]() -> float
        {
            ImFont *f = uiRenderer.GetPixelImFont();
            if (!f)
                f = settingsBodyFont;
            if (!f)
                return 0.0f;
            float pad = ImGui::GetStyle().FramePadding.x;
            float labelW = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, label).x;
            constexpr float minValueAreaW = 56.0f;
            constexpr float gap = 24.0f;
            return pad * 2.0f + labelW + gap + minValueAreaW;
        };

        struct DragEditState
        {
            bool tracking = false;
            bool requestEdit = false;
            bool editing = false;
            bool focusPending = false;
            ImVec2 startPos{};
        };
        auto dragState = std::make_shared<DragEditState>();
        auto display = std::make_shared<float>(0.f);
        auto textBuf = std::make_shared<std::array<char, 128>>();
        auto lenText = std::make_shared<bool>(false);

        line.imguiContent = [this, label, &paramMm, speedMm, minMm, maxMm, dragId,
                             onChange = std::move(onChange), settingsBodyFont, dragState, display, textBuf,
                             lenText](float w, float h, float iconOffset)
        {
            const LengthUnit du = LengthUnitFromIndex(settings.defaultLengthUnit);
            glm::vec4 tcLabel = Color::GetUIText(2);
            glm::vec4 tcValue = Color::GetUIText(0);
            float pad = ImGui::GetStyle().FramePadding.x;

            UIStyle::PushInputStyle(h, tcLabel);
            ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
            float originX = rowOrigin.x;

            ImFont *rowFont = FontOrInteractiveRow(uiRenderer, settingsBodyFont);
            float labelFontSz = rowFont ? rowFont->FontSize : ImGui::GetFontSize();

            bool showEdit = dragState->editing || dragState->focusPending;

            if (dragState->requestEdit)
            {
                *lenText = true;
                std::snprintf(textBuf->data(), textBuf->size(), "%.6g %s",
                              static_cast<double>(FromMillimeters(paramMm, du)),
                              LengthUnitAbbreviation(du));
                ImGui::SetKeyboardFocusHere();
                dragState->requestEdit = false;
                dragState->focusPending = true;
                showEdit = true;
            }

            float dragW, dragOffsetX;
            if (showEdit)
            {
                float labelTextW =
                    rowFont ? rowFont->CalcTextSizeA(labelFontSz, FLT_MAX, 0.0f, label).x : ImGui::CalcTextSize(label).x;
                float leftW = std::min(iconOffset + pad + labelTextW + pad * 2.5f, w * 0.6f);
                dragOffsetX = leftW;
                dragW = w - leftW;
            }
            else
            {
                dragOffsetX = 0.0f;
                dragW = w;
            }

            const float paramZoneRight = originX + dragOffsetX + dragW;

            bool changed = false;
            if (*lenText)
            {
                ImGui::SetCursorScreenPos(ImVec2(paramZoneRight - dragW, rowOrigin.y));
                ImGui::SetNextItemWidth(dragW);
                char textId[80];
                std::snprintf(textId, sizeof(textId), "##slen%s", dragId);
                (void)ImGui::InputText(textId, textBuf->data(), textBuf->size(),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
                UIStyle::DrawInputHoverTint(1);
                dragState->editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
                if (dragState->editing)
                    dragState->focusPending = false;
                if (*lenText && ImGui::IsItemDeactivated())
                {
                    float mm = paramMm;
                    if (TryParseLengthToMm(std::string_view(textBuf->data()), du, mm))
                    {
                        paramMm = mm;
                        *display = FromMillimeters(paramMm, du);
                        changed = true;
                    }
                    else
                    {
                        std::snprintf(textBuf->data(), textBuf->size(), "%.6g %s",
                                      static_cast<double>(FromMillimeters(paramMm, du)),
                                      LengthUnitAbbreviation(du));
                    }
                    *lenText = false;
                    dragState->focusPending = false;
                }
            }
            else
            {
                float itemW = dragW;
                if (showEdit && !*lenText)
                {
                    const float sw =
                        ImGui::CalcTextSize(LengthUnitAbbreviation(du)).x +
                        ImGui::GetStyle().ItemInnerSpacing.x * 2.0f;
                    itemW = std::max(dragW - sw, ImGui::GetFontSize() * 2.5f);
                }
                ImGui::SetCursorScreenPos(ImVec2(paramZoneRight - itemW, rowOrigin.y));
                ImGui::SetNextItemWidth(itemW);

                if (!ImGui::IsItemActive())
                    *display = FromMillimeters(paramMm, du);
                const float mmPer = MillimetersPerUnit(du);
                const float duSpeed = (mmPer > 0.0f) ? speedMm / mmPer : speedMm;
                const float dmin = FromMillimeters(minMm, du);
                const float dmax = FromMillimeters(maxMm, du);
                changed = ImGui::DragFloat(dragId, display.get(), duSpeed, dmin, dmax,
                                           showEdit ? "%.6g" : "");
                if (changed)
                    paramMm = ToMillimeters(*display, du);
                if (ImGui::IsItemDeactivatedAfterEdit())
                    paramMm = ToMillimeters(*display, du);
                UIStyle::DrawInputHoverTint(1);
                if (ImGui::IsItemActivated())
                    *lenText = false;

                dragState->editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
                if (dragState->editing)
                    dragState->focusPending = false;

                if (ImGui::IsItemActivated())
                {
                    dragState->tracking = true;
                    dragState->startPos = ImGui::GetIO().MousePos;
                }
                if (dragState->tracking && !ImGui::IsItemActive())
                {
                    ImVec2 ep = ImGui::GetIO().MousePos;
                    float dx = ep.x - dragState->startPos.x;
                    float dy = ep.y - dragState->startPos.y;
                    if (dx * dx + dy * dy < 9.0f)
                        dragState->requestEdit = true;
                    dragState->tracking = false;
                }
            }

            showEdit = dragState->editing || dragState->focusPending;
            if (*lenText)
                showEdit = true;

            ImDrawList *dl = ImGui::GetWindowDrawList();
            float bottom = ImGui::GetItemRectMax().y - ImGui::GetStyle().FramePadding.y;

            ImU32 labelCol = ImGui::GetColorU32(ImVec4(tcLabel.r, tcLabel.g, tcLabel.b, tcLabel.a));
            {
                float ty_label = bottom - labelFontSz;
                if (rowFont)
                    dl->AddText(rowFont, labelFontSz, ImVec2(originX + iconOffset + pad, ty_label), labelCol, label);
                else
                    dl->AddText(ImVec2(originX + iconOffset + pad, ty_label), labelCol, label);
            }

            if (!showEdit)
            {
                char valBuf[48];
                FormatLengthMmForDisplay(valBuf, sizeof(valBuf), paramMm, du);
                ImVec2 vs =
                    rowFont ? rowFont->CalcTextSizeA(labelFontSz, FLT_MAX, 0.0f, valBuf) : ImGui::CalcTextSize(valBuf);
                float ty_value = bottom - labelFontSz;
                ImU32 valCol = ImGui::GetColorU32(ImVec4(tcValue.r, tcValue.g, tcValue.b, tcValue.a));
                if (rowFont)
                    dl->AddText(rowFont, labelFontSz, ImVec2(paramZoneRight - pad - vs.x, ty_value), valCol, valBuf);
                else
                    dl->AddText(ImVec2(paramZoneRight - pad - vs.x, ty_value), valCol, valBuf);
            }
            else if (!*lenText)
            {
                const char *abbr = LengthUnitAbbreviation(du);
                ImVec2 us =
                    rowFont ? rowFont->CalcTextSizeA(labelFontSz, FLT_MAX, 0.0f, abbr) : ImGui::CalcTextSize(abbr);
                float ty_value = bottom - labelFontSz;
                ImU32 dimU = ImGui::GetColorU32(ImVec4(tcValue.r, tcValue.g, tcValue.b, tcValue.a));
                if (rowFont)
                    dl->AddText(rowFont, labelFontSz, ImVec2(paramZoneRight - pad - us.x, ty_value), dimU, abbr);
                else
                    dl->AddText(ImVec2(paramZoneRight - pad - us.x, ty_value), dimU, abbr);
            }

            if (changed)
                onChange();
            UIStyle::PopInputStyle();
        };
    };

    // Settings panel was created early in InitUI (left edge; toolbar sits to its right).
    // Add sections to the existing uiSettings panel here.

    // ── Appearance ────────────────────────────────────────────────────────────
    Section &appearanceSection = uiSettings->AddSection("Appearance");
    appearanceSection.header = Header{"Appearance", 1.0f, 2};
    appearanceSection.tightHeader = true;
    appearanceSection.children.reserve(1);

    // All appearance rows in one paragraph — no splitters between them.
    Paragraph &appearancePara = appearanceSection.AddParagraph("AppearanceRows");
    // Theme + Accent + Contrast are appended below; reserve all so stored select pointers remain valid.
    appearancePara.values.reserve(3);

    // Theme selector: System / Light / Dark pill
    {
        SectionLine &themeSelect = appearancePara.values.emplace_back();
        themeSelect.text = "Theme";
        Select sel;
        sel.options = {
            {"System", Icons::ThemeSystem()},
            {"Light", Icons::ThemeLight()},
            {"Dark", Icons::ThemeDark()},
        };
        sel.activeIndex = static_cast<int>(themeMode);
        sel.onChange = [this](int i)
        {
            themeMode = static_cast<ThemeMode>(i);
            ApplyTheme();
            uiRenderer.MarkDirty();
        };
        themeSelect.select = std::move(sel);
        uiAppearanceThemeSelect = &themeSelect.select.value();
    }

    // Accent selector: System / Color pill — native select, identical layout to Theme.
    // onActiveClick on zone 1 (Color) opens the HSV picker popup; postDraw hosts it in the same window.
    {
        SectionLine &accentSel = appearancePara.values.emplace_back();
        accentSel.text = "Accent";
        Select sel;
        sel.options = {
            {"System", Icons::ThemeSystem()},
            {"Custom", Icons::AccentCustom()},
        };
        sel.activeIndex = settingsAccentUseSystem ? 0 : 1;
        sel.onChange = [this](int i)
        {
            settingsAccentUseSystem = (i == 0);
            if (settingsAccentUseSystem)
            {
                float hue, sat;
                if (SystemAccent::GetHueSat(hue, sat))
                {
                    // Keep saved custom hue/sat for when user switches back to Custom — only drive live color from OS.
                    Color::SetAccent(hue, sat);
                    uiRenderer.MarkDirty();
                    if (scene && (!scene->solids.empty() || !scene->faces.empty()))
                        MarkStyleDirty();
                    MarkPickDirty();
                }
            }
            else
            {
                Color::SetAccent(settingsAccentHue, settingsAccentSat);
                uiRenderer.MarkDirty();
                if (scene && (!scene->solids.empty() || !scene->faces.empty()))
                    MarkStyleDirty();
                MarkPickDirty();
            }
        };
        sel.onActiveClick = [this]()
        {
            if (!settingsAccentUseSystem) // only Custom zone can be active here
                settingsOpenAccentPicker = true;
        };
        sel.postDraw = [this]()
        {
            if (settingsOpenAccentPicker)
            {
                ImGui::OpenPopup("##accentPicker");
                settingsOpenAccentPicker = false;
            }
            if (ImGui::BeginPopup("##accentPicker"))
            {
                float hsv[3] = {settingsAccentHue / 360.0f, settingsAccentSat, 1.0f};
                float col4[4] = {};
                ImGui::ColorConvertHSVtoRGB(hsv[0], hsv[1], hsv[2], col4[0], col4[1], col4[2]);
                col4[3] = 1.0f;
                if (ImGui::ColorPicker4("##picker", col4,
                                        ImGuiColorEditFlags_NoAlpha |
                                            ImGuiColorEditFlags_DisplayHSV |
                                            ImGuiColorEditFlags_InputRGB))
                {
                    float h2, s2, v2;
                    ImGui::ColorConvertRGBtoHSV(col4[0], col4[1], col4[2], h2, s2, v2);
                    settingsAccentHue = h2 * 360.0f;
                    settingsAccentSat = s2;
                    Color::SetAccent(settingsAccentHue, settingsAccentSat);
                    uiRenderer.MarkDirty();
                    if (scene && (!scene->solids.empty() || !scene->faces.empty()))
                        MarkStyleDirty();
                    MarkPickDirty();
                }
                ImGui::EndPopup();
            }
        };
        accentSel.select = std::move(sel);
        uiAppearanceAccentSelect = &accentSel.select.value();
    }

    makeSettingsDrag(appearancePara.values.emplace_back(), "Contrast", UserTuning::contrast,
                     0.01f, 0.0f, 1.0f, "%.2f", "##contrast",
                     [this]()
                     {
                         UserTuning::DeriveFromContrast();
                         Color::SetUiDepthStep(UserTuning::uiDepthStep);
                         viewportRenderer.RegenerateGrid();
                         uiRenderer.MarkDirty();
                         if (scene && (!scene->solids.empty() || !scene->faces.empty()))
                            MarkStyleDirty();
                        MarkPickDirty();
                     });

    // ── Viewport ──────────────────────────────────────────────────────────────
    Section &viewportSection = uiSettings->AddSection("Viewport");
    viewportSection.header = Header{"Viewport", 1.0f, 2};
    viewportSection.tightHeader = true;
    viewportSection.children.reserve(1);

    Paragraph &gridPara = viewportSection.AddParagraph("GridSize");
    gridPara.values.reserve(3);

    {
        SectionLine &unitSel = gridPara.values.emplace_back();
        unitSel.text = "Length unit";
        Select sel;
        sel.textOnly = true;
        sel.options = {
            {"mm", {}},
            {"cm", {}},
            {"in", {}},
            {"ft", {}},
        };
        sel.activeIndex = std::clamp(settings.defaultLengthUnit, 0, 3);
        sel.onChange = [this](int i)
        {
            settings.defaultLengthUnit = std::clamp(i, 0, 3);
            SyncGridLayoutFromSettings();
            SaveSettings();
            uiRenderer.MarkDirty();
            renderDirty = true;
        };
        unitSel.select = std::move(sel);
        uiDefaultLengthUnitSelect = &unitSel.select.value();
    }

    makeSettingsDrag(gridPara.values.emplace_back(), "Grid size", settings.gridCellsAlongAxis,
                     1.0f, 4.0f, 8192.0f, "%.0f", "##gridCellsAlong",
                     [this]()
                     {
                         SyncGridLayoutFromSettings();
                         SaveSettings();
                     });

    makeSettingsDrag(gridPara.values.emplace_back(), "Low angle fade",
                     settings.gridPlaneTiltMinOpacity,
                     0.01f, 0.0f, 1.0f, "%.2f", "##gridPlaneTiltMin",
                     [this]()
                     {
                         settings.gridPlaneTiltMinOpacity =
                             std::clamp(settings.gridPlaneTiltMinOpacity, 0.0f, 1.0f);
                         viewportRenderer.SetGridPlaneTiltMinOpacity(settings.gridPlaneTiltMinOpacity);
                         SaveSettings();
                         renderDirty = true;
                     });

    // ── Navigation ───────────────────────────────────────────────────────────────────────────
    Section &navSection = uiSettings->AddSection("Navigation");
    navSection.header = Header{"Navigation", 1.0f, 2};
    navSection.tightHeader = true;
    navSection.children.reserve(1);

    Paragraph &sensPara = navSection.AddParagraph("MouseSens");
    sensPara.values.reserve(1);
    makeSettingsDrag(sensPara.values.emplace_back(), "Sensitivity", mouseSensitivity,
                     1.0f, 1.0f, 500.0f, "%.0f", "##mouseSens",
                     [this]()
                     { renderDirty = true; });
    makeSettingsDrag(sensPara.values.emplace_back(), "Snap", UserTuning::snap,
                     0.01f, 0.0f, 1.0f, "%.2f", "##snapMaster",
                     [this]()
                     {
                         UserTuning::DeriveFromSnap();
                         renderDirty = true;
                     });

    // ── Calibrate panel ───────────────────────────────────────────────────────
    {
        ToolPanelDef calibDef;
        calibDef.id = "Calibrate";
        calibDef.name = "Calibrate";
        calibDef.description = "Calibrate 3D printer accuracy by making measurements.";
        calibDef.flattenParameters = false;
        calibDef.showSectionHeaders = true;
        calibDef.sectionHeadersCollapsible = false;
        calibDef.parametersSectionTitle = "Parameters";
        calibDef.hasCalculator = true;
        calibDef.maxCalculatorLines = 1;
        calibDef.calculatorSectionTitle = "Result";

        // ── Prerequisites ──────────────────────────────────────────────────
        calibDef.prerequisites.reserve(3);
        calibDef.prerequisites.push_back({"CalibImport", "Import a file", "",
                                          Icons::CheckBox(&calibStepImport), false, true,
                                          [this]()
                                          { DoFileImport(); }});
        calibDef.prerequisites.push_back({"CalibPoint1", "Plot measurement point", "to measure against",
                                          Icons::CheckBox(&calibStepPoint1), false, false});
        calibDef.prerequisites.push_back({"CalibPoint2", "Plot measurement point", "parallel point to first selection",
                                          Icons::CheckBox(&calibStepPoint2), false, false});

        // ── Parameters — print measurement InputFloat ──────────────────────
        // TODO(Calibrate): Inline CAD nominal span (and pick/span status when needed) on this row so
        // CalibDerived can stay compensation-only; see CalibDerived for messages still on the row below.
        {
            ParameterDef pm;
            pm.id = "CalibMeasure";
            pm.line.iconDraw = Icons::StepDot(&calibStepMeasure);
            pm.line.getMinContentWidthPx = [this, settingsBodyFont]() -> float
            {
                ImFont *f = uiRenderer.GetPixelImFont();
                if (!f)
                    f = settingsBodyFont;
                if (!f)
                    return 0.0f;
                float pad = ImGui::GetStyle().FramePadding.x;
                float labelW = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, "Print measurement").x;
                return pad * 2.0f + labelW + 24.0f + 48.0f;
            };
            auto pmEditing = std::make_shared<bool>(false);
            auto calibFocusRequest = std::make_shared<bool>(false);
            auto calibEditHadFocus = std::make_shared<bool>(false);
            auto calibText = std::make_shared<std::array<char, 128>>();
            calibText->data()[0] = '\0';
            pm.line.imguiContent = [this, settingsBodyFont, pmEditing, calibText, calibFocusRequest, calibEditHadFocus](float w, float h, float iconOffset)
            {
                (void)iconOffset;
                const LengthUnit du = LengthUnitFromIndex(settings.defaultLengthUnit);
                glm::vec4 tcLabel = Color::GetUIText(2);
                glm::vec4 tcValue = Color::GetUIText(0);
                float pad = ImGui::GetStyle().FramePadding.x;

                UIStyle::PushInputStyle(h, tcLabel);
                ImVec2 rowOrigin = ImGui::GetCursorScreenPos();

                ImFont *rowFont = FontOrInteractiveRow(uiRenderer, settingsBodyFont);
                const float fs = rowFont ? rowFont->FontSize : ImGui::GetFontSize();
                float labelTextW =
                    rowFont ? rowFont->CalcTextSizeA(fs, FLT_MAX, 0.0f, "Print measurement").x : ImGui::CalcTextSize("Print measurement").x;
                // Keep the input visually closer to the label (was too far right).
                float leftW = pad + labelTextW + pad * 1.25f;
                float inputW = w - leftW;

                bool changed = false;

                // Idle = hit target + drawn readout; edit = real InputText only. A ReadOnly InputText
                // with hidden text was crashing on focus loss in this nested ToolPanel window.
                if (*pmEditing)
                {
                    if (*calibFocusRequest)
                    {
                        ImGui::SetKeyboardFocusHere();
                        *calibFocusRequest = false;
                    }
                    ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x + leftW, rowOrigin.y));
                    ImGui::SetNextItemWidth(inputW);
                    const bool enterCommit = ImGui::InputText("##calibMeasured", calibText->data(), calibText->size(),
                                                              ImGuiInputTextFlags_EnterReturnsTrue);
                    UIStyle::DrawInputHoverTint(1);

                    if (ImGui::IsItemActivated() || ImGui::IsItemActive())
                        *calibEditHadFocus = true;
                    // Replacing InvisibleButton with InputText can yield IsItemDeactivated on the same
                    // transition before the field ever becomes active — that was closing edit immediately.
                    const bool leaveEdit = enterCommit || (*calibEditHadFocus && ImGui::IsItemDeactivated());
                    if (*pmEditing && leaveEdit)
                    {
                        float mm = calibMeasured;
                        if (TryParseLengthToMm(std::string_view(calibText->data()), du, mm))
                        {
                            calibMeasured = mm;
                            changed = true;
                        }
                        else
                        {
                            std::snprintf(calibText->data(), calibText->size(), "%.6g %s",
                                          static_cast<double>(FromMillimeters(calibMeasured, du)),
                                          LengthUnitAbbreviation(du));
                        }
                        *pmEditing = false;
                        *calibEditHadFocus = false;
                    }
                }
                else
                {
                    std::snprintf(calibText->data(), calibText->size(), "%.6g %s",
                                  static_cast<double>(FromMillimeters(calibMeasured, du)),
                                  LengthUnitAbbreviation(du));
                    ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x, rowOrigin.y));
                    const float hitH = std::max(h, ImGui::GetFrameHeight());
                    ImGui::InvisibleButton("##calibMeasured", ImVec2(w, hitH));
                    UIStyle::DrawInputHoverTint(1);
                    if (ImGui::IsItemClicked())
                    {
                        *pmEditing = true;
                        *calibFocusRequest = true;
                        *calibEditHadFocus = false;
                    }
                }

                ImDrawList *dl = ImGui::GetWindowDrawList();
                float itemBottom = ImGui::GetItemRectMax().y;
                const float cellRight = ImGui::GetItemRectMax().x;
                float labelTextH =
                    rowFont ? rowFont->CalcTextSizeA(fs, FLT_MAX, 0.0f, "Print measurement").y : ImGui::CalcTextSize("Print measurement").y;
                ImU32 labelCol = ImGui::GetColorU32(ImVec4(tcLabel.r, tcLabel.g, tcLabel.b, tcLabel.a));
                if (rowFont)
                    dl->AddText(rowFont, fs, ImVec2(rowOrigin.x + pad, itemBottom - labelTextH), labelCol, "Print measurement");
                else
                    dl->AddText(ImVec2(rowOrigin.x + pad, itemBottom - labelTextH), labelCol, "Print measurement");

                // Readout / edit: keep inside the input frame; full string includes unit when idle.
                if (!*pmEditing)
                {
                    char valueBuf[48];
                    FormatLengthMmForDisplay(valueBuf, sizeof(valueBuf), calibMeasured, du);
                    ImFont *valFont = rowFont;
                    if (!valFont && ImGui::GetIO().Fonts && ImGui::GetIO().Fonts->Fonts.Size > 0)
                        valFont = ImGui::GetIO().Fonts->Fonts[0];
                    if (valFont)
                    {
                        ImVec2 vs = valFont->CalcTextSizeA(valFont->FontSize, FLT_MAX, 0.0f, valueBuf);
                        ImU32 unitCol = ImGui::GetColorU32(ImVec4(tcValue.r, tcValue.g, tcValue.b, tcValue.a));
                        dl->AddText(valFont, valFont->FontSize, ImVec2(cellRight - pad - vs.x, itemBottom - vs.y), unitCol, valueBuf);
                    }
                }

                if (changed)
                {
                    RefreshCalibCompensation();
                    uiRenderer.MarkDirty();
                    renderDirty = true;
                }
                UIStyle::PopInputStyle();
            };
            calibDef.parameters.push_back(std::move(pm));
        }

        ParameterDef pmDer;
            pmDer.id = "CalibDerived";
            pmDer.line.iconDraw = Icons::StepDot(&calibStepMeasure);
            pmDer.line.getMinContentWidthPx = [this, settingsBodyFont]() -> float
            {
                ImFont *f = uiRenderer.GetPixelImFont();
                if (!f)
                    f = settingsBodyFont;
                if (!f)
                    return 0.0f;
                float pad = ImGui::GetStyle().FramePadding.x;
                float labelW = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f,
                                                "First-layer excess (printed \xe2\x88\x92 CAD)").x;
                return pad * 2.0f + labelW + 72.0f;
            };
            pmDer.line.imguiContent = [this, settingsBodyFont](float w, float h, float iconOffset)
            {
                (void)h;
                (void)iconOffset;
                glm::vec4 tcLabel = Color::GetUIText(2);
                glm::vec4 tcValue = Color::GetUIText(0);
                float pad = ImGui::GetStyle().FramePadding.x;
                ImVec2 row0 = ImGui::GetCursorScreenPos();
                ImDrawList *dl = ImGui::GetWindowDrawList();

                ImFont *rowFont = FontOrInteractiveRow(uiRenderer, settingsBodyFont);
                const float fs = rowFont ? rowFont->FontSize : ImGui::GetFontSize();
                float lh = rowFont ? rowFont->CalcTextSizeA(fs, FLT_MAX, 0.0f, "Mg").y : ImGui::GetTextLineHeight();
                const float rowHitH = std::max(lh * 1.35f, ImGui::GetFrameHeight());
                const float y0 = row0.y + pad * 0.25f;

                const bool missingFaces =
                    !CalibSlotHasPick(calibFacePoint1, calibEdgePoint1) ||
                    !CalibSlotHasPick(calibFacePoint2, calibEdgePoint2);
                const bool spanBad = !missingFaces && calibNominal <= 1e-5f;
                const bool importDone = calibStepImport == Icons::StepState::Done;
                const bool firstFaceDone = calibStepPoint1 == Icons::StepState::Done;

                if (missingFaces)
                {
                    // The derived row should not reserve result space until a full span exists.
                    if (!importDone || !firstFaceDone)
                    {
                        ImGui::Dummy(ImVec2(w, pad));
                        return;
                    }
                    ImGui::Dummy(ImVec2(w, pad));
                    return;
                }
                if (spanBad)
                {
                    CalibDrawCopyableResultRow(dl, row0.x, y0, w, rowHitH, pad, nullptr, tcLabel, tcValue,
                                               "Could not estimate span (try parallel faces).", "", "spanBad");
                    ImGui::Dummy(ImVec2(w, rowHitH + pad));
                    return;
                }

                float y = y0;
                if (calibCompensationValid && calibWorkflow != CalibWorkflow::None)
                {
                    char valB[48] = {};
                    const char *lab = "";
                    switch (calibWorkflow)
                    {
                    case CalibWorkflow::Contour:
                        lab = "Shrinkage";
                        std::snprintf(valB, sizeof(valB), "%.4f", calibContourScale);
                        CalibDrawCopyableResultRow(dl, row0.x, y, w, rowHitH, pad, nullptr, tcLabel, tcValue,
                                                   lab, valB, "contour");
                        break;
                    case CalibWorkflow::Hole:
                        lab = "Hole Radius Offset";
                        std::snprintf(valB, sizeof(valB), "%.3f mm", calibHoleOffsetMm);
                        CalibDrawCopyableResultRow(dl, row0.x, y, w, rowHitH, pad, nullptr, tcLabel, tcValue,
                                                   lab, valB, "hole");
                        break;
                    case CalibWorkflow::ElephantFoot:
                        lab = "First-layer excess (printed \xe2\x88\x92 CAD)";
                        std::snprintf(valB, sizeof(valB), "%.3f mm", calibElephantFootMm);
                        CalibDrawCopyableResultRow(dl, row0.x, y, w, rowHitH, pad, nullptr, tcLabel, tcValue,
                                                   lab, valB, "elephant");
                        break;
                    default:
                        break;
                    }
                }
                else
                {
                    CalibDrawCopyableResultRow(dl, row0.x, y, w, rowHitH, pad, nullptr, tcLabel, tcValue,
                                               "Adjust print measurement to compute compensation.", "", "hint");
                }

                ImGui::Dummy(ImVec2(w, rowHitH + pad));
            };
        // `pmDer` is moved into the Calculator ("Result") section after `BuildToolPanel`.

        RootPanel calibPanel = BuildToolPanel(calibDef);
        calibPanel.visible = false;
        calibPanel.leftAnchor = PanelAnchor{uiToolbar, PanelAnchor::Right};
        calibPanel.topAnchor = PanelAnchor{uiFiles, PanelAnchor::Bottom};
        uiCalibrate = &uiRenderer.AddPanel(calibPanel);
        // AddPanel copies `calibPanel`; vector capacity hint on the local copy is not preserved.
        // Reserve on the live panel before storing child pointers.
        uiCalibrate->children.reserve(uiCalibrate->children.size() + 1);

        // ── Paragraph pointers for live state mutation ──────────────────────
        Section *prereqs = FindSection(*uiCalibrate, "Prerequisites");
        calibSec_Prerequisites = prereqs;
        calibPara_Import = &prereqs->children[0];
        calibPara_Point1 = &prereqs->children[1];
        calibPara_Point2 = &prereqs->children[2];
        calibLine_Point1Primary = &prereqs->children[1].values[0];
        calibLine_Point2Primary = &prereqs->children[2].values[0];

        calibSec_Parameters = FindSection(*uiCalibrate, "Parameters");
        calibSec_Result = FindSection(*uiCalibrate, "Calculator");
        if (calibSec_Parameters && !calibSec_Parameters->children.empty())
        {
            calibSec_Parameters->noChildSplitters = false;
            for (Paragraph &child : calibSec_Parameters->children)
                child.margin = UIGrid::GAP * UIElement::INSET_RATIO * 0.5f;
            calibPara_Measure = &calibSec_Parameters->children[0];
        }
        else
        {
            auto findRootParagraph = [this](const std::string &id) -> Paragraph *
            {
                for (ChildElement &child : uiCalibrate->children)
                {
                    if (Paragraph *p = std::get_if<Paragraph>(&child); p && p->id == id)
                        return p;
                }
                return nullptr;
            };
            calibPara_Measure = findRootParagraph("CalibMeasure");
            if (calibPara_Measure)
                calibPara_Measure->margin = UIGrid::GAP * UIElement::INSET_RATIO * 0.5f;
        }

        if (calibSec_Result)
        {
            calibSec_Result->noChildSplitters = false;
            Paragraph &derivedPara = calibSec_Result->AddParagraph("CalibDerived");
            derivedPara.values.reserve(1);
            derivedPara.values.push_back(std::move(pmDer.line));
            calibPara_Derived = &derivedPara;
            calibPara_Derived->margin = UIGrid::GAP * UIElement::INSET_RATIO * 0.5f;
        }
        else
        {
            calibPara_Derived = nullptr;
        }

        // Click handlers — selecting a point prerequisite deselects the other.
        auto selectPoint1 = [this]()
        {
            calibPara_Point1->selected = true;
            calibPara_Point2->selected = false;
            uiRenderer.MarkDirty();
            MarkPickDirty();
        };
        auto selectPoint2 = [this]()
        {
            calibPara_Point2->selected = true;
            calibPara_Point1->selected = false;
            uiRenderer.MarkDirty();
            MarkPickDirty();
        };
        calibPara_Point1->onClick        = selectPoint1;
        calibLine_Point1Primary->onClick = selectPoint1;
        if (calibPara_Point1->values.size() > 1)
            calibPara_Point1->values[1].onClick = selectPoint1;
        calibPara_Point2->onClick        = selectPoint2;
        calibLine_Point2Primary->onClick = selectPoint2;
        if (calibPara_Point2->values.size() > 1)
            calibPara_Point2->values[1].onClick = selectPoint2;

        // Point1 and Point2 are hidden until a file is imported
        calibPara_Point1->visible = false;
        calibPara_Point2->visible = false;
        if (calibSec_Parameters)
            calibSec_Parameters->visible = false;
        if (calibSec_Result)
            calibSec_Result->visible = false;
        if (calibPara_Measure)
            calibPara_Measure->visible = false;
        if (calibPara_Derived)
            calibPara_Derived->visible = false;

        uiCalibrateProcessing = &uiCalibrate->AddParagraph("Processing");
        uiCalibrateProcessing->visible = false;
        uiCalibrateProcessing->dimFill = true;
        uiCalibrateProcessing->padding = UIGrid::GAP * UIElement::INSET_RATIO * 0.85f;
        uiCalibrateProcessing->values.reserve(1);
        {
            SectionLine &line = uiCalibrateProcessing->values.emplace_back();
            line.text = "Refreshing calibration...";
            line.textDepth = 2;
        }

        RefreshCalibDerivedRowVisible();
    }

    {
        ToolPanelDef structDef;
        structDef.id = "Structure";
        structDef.name = "Structure";
        structDef.description =
            "Preview adaptive internal bracing. Changing the solid mesh is planned for a later release.";
        structDef.flattenParameters = true;
        structDef.parameters.reserve(4);

        ParameterDef pmPreviewShow;
        pmPreviewShow.id = "StructPreviewShow";
        pmPreviewShow.line.getMinContentWidthPx = [settingsBodyFont]() -> float
        {
            ImFont *f = settingsBodyFont;
            if (!f)
                f = ImGui::GetFont();
            const float pad = ImGui::GetStyle().FramePadding.x;
            const float tw =
                f ? f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, "Structure preview lines").x : 200.0f;
            return pad * 2.0f + tw + 28.0f;
        };
        pmPreviewShow.line.imguiContent = [this](float w, float h, float)
        {
            (void)w;
            (void)h;
            if (ImGui::Checkbox("Structure preview lines", &structurePreviewEnabled))
            {
                RefreshStructurePreviewForRenderer();
                MarkGeometryDirtyAll();
                renderDirty = true;
            }
        };
        structDef.parameters.push_back(std::move(pmPreviewShow));

        ParameterDef pmPreviewPattern;
        pmPreviewPattern.id = "StructPreviewPattern";
        pmPreviewPattern.line.getMinContentWidthPx = [settingsBodyFont]() -> float
        {
            ImFont *f = settingsBodyFont;
            if (!f)
                f = ImGui::GetFont();
            const float pad = ImGui::GetStyle().FramePadding.x;
            const float tw =
                f ? std::max(f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, "Adjacent face midpoints (3D diamond)").x,
                             f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, "Preview pattern").x)
                  : 320.0f;
            return pad * 4.0f + tw + 60.0f;
        };
        pmPreviewPattern.line.imguiContent = [this](float w, float h, float)
        {
            (void)w;
            (void)h;
            int pat = static_cast<int>(structurePreviewPattern);
            const char *items[] = {"Adjacent face midpoints (3D diamond)", "Center -> bbox (heuristic)"};
            constexpr int itemCount = static_cast<int>(sizeof(items) / sizeof(items[0]));
            if (ImGui::Combo("Preview pattern", &pat, items, itemCount))
            {
                structurePreviewPattern =
                    static_cast<StructurePreview::PreviewPattern>(std::clamp(pat, 0, itemCount - 1));
                RefreshStructurePreviewForRenderer();
                MarkGeometryDirtyAll();
                renderDirty = true;
            }
        };
        structDef.parameters.push_back(std::move(pmPreviewPattern));

        ParameterDef pmTranslucent;
        pmTranslucent.id = "StructTranslucent";
        pmTranslucent.line.getMinContentWidthPx = [settingsBodyFont]() -> float
        {
            ImFont *f = settingsBodyFont;
            if (!f)
                f = ImGui::GetFont();
            const float pad = ImGui::GetStyle().FramePadding.x;
            const float tw =
                f ? f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, "Translucent solid shell (see inside)").x
                  : 280.0f;
            return pad * 2.0f + tw + 28.0f;
        };
        pmTranslucent.line.imguiContent = [this](float w, float h, float)
        {
            (void)w;
            (void)h;
            if (ImGui::Checkbox("Translucent solid shell (see inside)", &structureTranslucentShellEnabled))
                renderDirty = true;
        };
        structDef.parameters.push_back(std::move(pmTranslucent));

        ParameterDef pmShell;
        pmShell.id = "StructShell";
        pmShell.line.getMinContentWidthPx = [settingsBodyFont]() -> float
        {
            ImFont *f = settingsBodyFont;
            if (!f)
                f = ImGui::GetFont();
            const float pad = ImGui::GetStyle().FramePadding.x;
            const float tw = f ? f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f,
                                                "Inner offset walls / bending stiffness (soon)")
                                 .x
                           : 280.0f;
            return pad * 2.0f + tw + 28.0f;
        };
        pmShell.line.imguiContent = [this](float w, float h, float)
        {
            (void)w;
            (void)h;
            ImGui::BeginDisabled();
            ImGui::Checkbox("Inner offset walls / bending stiffness (soon)", &structureInnerShellRowUnchecked);
            ImGui::EndDisabled();
        };
        structDef.parameters.push_back(std::move(pmShell));

        RootPanel structPanel = BuildToolPanel(structDef);
        structPanel.visible = false;
        structPanel.leftAnchor = PanelAnchor{uiToolbar, PanelAnchor::Right};
        structPanel.topAnchor = PanelAnchor{uiFiles, PanelAnchor::Bottom};
        uiStructure = &uiRenderer.AddPanel(structPanel);
    }

    SyncToolbarToolVisualState();
    RefreshUIMinWindowSize();
}

void Display::RefreshStructurePreviewForRenderer()
{
    std::vector<std::pair<glm::vec3, glm::vec3>> segs;
    if (scene != nullptr && activeTool == ActiveTool::Structure && uiStructure != nullptr && uiStructure->visible &&
        structurePreviewEnabled && !scene->solids.empty())
    {
        switch (structurePreviewPattern)
        {
        case StructurePreview::PreviewPattern::AdjacentFaceMidpoints:
            StructurePreview::BuildAdjacentFaceMidpoints(*scene, segs);
            break;
        case StructurePreview::PreviewPattern::CenterStrutsBBox:
            StructurePreview::BuildCenterStruts(*scene, segs);
            break;
        }
    }
    renderer.SetStructurePreviewSegments(std::move(segs));
}

void Display::RefreshUIMinWindowSize()
{
    if (!window)
        return;
    uiRenderer.ComputeMinGridSize();
    const auto &grid = uiRenderer.GetGrid();
    SDL_SetWindowMinimumSize(window, grid.MinWidthPixels(), grid.MinHeightPixels());
}