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
#include <unordered_set>
#include <queue>
#include <cstdio>
#include <random>
#include "logic/Import/OBJImport.hpp"
#include "logic/Import/ThreeMFImport.hpp"
#include "input/FileImport.hpp"
#include "rendering/ScenePick.hpp"
#include "Geometry/Edge.hpp"
#include "CalibNominal.hpp"
#include "CalibCompensation.hpp"
#include <cmath>
#include <limits>

#include "ViewportDepthExperiments.hpp"
#include "RenderingExperiments.hpp"
#include "scene/scene.hpp"

namespace
{

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
    const float pad = std::max(50.0f, span * 0.1f);
    camera.nearPlane = minZ - pad;
    camera.farPlane = maxZ + pad;
}
} // namespace

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
        UpdateScene();
    renderDirty = true;
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
    RefreshStatusStripIdleText();
    SDL_AddEventWatch(ResizeEventWatcher, this);

    LOG_VOID("Initialized display");
}

SDL_Window *Display::InitWindow(int16_t width, int16_t height, const char *title)
{
    // macOS: built-in trackpad reports 2-finger drags as touch, not as mouse wheel, so we can
    // map them to pan. Physical scroll wheels still use SDL_EVENT_MOUSE_WHEEL.
    SDL_SetHint(SDL_HINT_TRACKPAD_IS_TOUCH_ONLY, "1");
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
    ApplyTheme();

    // Viewport
    Color::GRID_EXTENT = loaded.gridExtent;
    lastSyncedAxisWorldHalfExtent = std::numeric_limits<float>::quiet_NaN();
    (void)SyncViewportAxisForDepthClip();

    // Navigation
    mouseSensitivity = loaded.mouseSensitivity;

    // Re-run analysis with restored parameters.
    RebuildAnalysis();
    renderDirty = true;

    // Settings UI was built before load — sync pill indices to restored state.
    if (uiAppearanceThemeSelect)
        uiAppearanceThemeSelect->activeIndex = static_cast<int>(themeMode);
    if (uiAppearanceAccentSelect)
        uiAppearanceAccentSelect->activeIndex = settingsAccentUseSystem ? 0 : 1;
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
    settings.gridExtent = Color::GRID_EXTENT;
    settings.mouseSensitivity = mouseSensitivity;
    settings.Save(Settings::DefaultPath());
}

void Display::RebuildAnalysis()
{
    Analysis::Instance().Clear();
    Analysis::Instance().AddFaceAnalysis(std::make_unique<Overhang>(overhangAngle));
    Analysis::Instance().AddSolidAnalysis(std::make_unique<SmallFeature>(layerHeight, minFeatureSize));
    Analysis::Instance().AddSolidAnalysis(std::make_unique<ThinSection>(layerHeight, thinMinWidth));
    Analysis::Instance().AddEdgeAnalysis(std::make_unique<SharpCorner>(sharpCornerAngle));
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

    // Mark only the solid surface pixels in the stencil buffer (value = 1).
    // Lines are excluded — their geometry-shader quads extend beyond silhouettes
    // and would bleed into the stencil, incorrectly clipping axes.
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    renderer.RenderPatches();
    renderer.RenderPickHighlight();
    if (cullOpaqueTriangles)
        glDisable(GL_CULL_FACE);

    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP); // stop writing before lines
    if (!RenderingExperiments::kDebugSkipSceneWireframe)
        renderer.RenderWireframe();
    // Calibrate edge picks: thick accent lines on top of wireframe (same GS line pipeline).
    renderer.RenderPickHighlightLines(6.0f);

    // Grid after solid + wireframe: stencil==0 only so lines do not bleed onto filled surfaces;
    // clip Z bias still keeps axes > grid > scene where stencil allows.
    viewportRenderer.Render();

    viewportRenderer.RenderAxes();

    glDisable(GL_STENCIL_TEST);

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
        calibStepPoint1 = Icons::StepState::Active;
        calibStepPoint2 = Icons::StepState::Active;
        if (calibPara_Point1)
            calibPara_Point1->selected = true;
        if (calibPara_Point2)
            calibPara_Point2->selected = false;
        uiRenderer.MarkDirty();
        bool nowAnalysis = (activeTool == ActiveTool::Analysis);
        uiAnalysis->visible = nowAnalysis;
        uiCalibrate->visible = !nowAnalysis;
        if (analysisEnabled != nowAnalysis)
        {
            analysisEnabled = nowAnalysis;
            UpdateScene();
        }
        uiRenderer.MarkDirty();
        SyncToolbarToolVisualState();
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
    sceneDirty = true;
    renderDirty = true;
}

void Display::Frame()
{
    ProcessDeferredImportIfAny();

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

    if (sceneDirty)
    {
        bool hasModel = !scene->solids.empty() || !scene->faces.empty();

        // Toggle Analysis panel sections based on model presence
        if (uiImportPara)
            uiImportPara->visible = !hasModel;
        if (uiResult)
            uiResult->visible = hasModel;
        if (uiVerdict)
            uiVerdict->visible = hasModel;
        uiRenderer.MarkDirty();

        AnalysisResults results;
        if (analysisEnabled)
            results = Analysis::Instance().AnalyzeScene(scene);

        renderer.UpdateScene(scene, analysisEnabled ? &results : nullptr);

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
            hoverPickFace = nullptr;
        if (hoverPickEdge != nullptr && !stillPickableEdge(hoverPickEdge))
            hoverPickEdge = nullptr;

        bool calibPickInvalidated = false;
        if (calibFacePoint1 != nullptr && !stillPickable(calibFacePoint1))
        {
            calibFacePoint1 = nullptr;
            calibStepPoint1 = Icons::StepState::Active;
            calibPickInvalidated = true;
        }
        if (calibEdgePoint1 != nullptr && !stillPickableEdge(calibEdgePoint1))
        {
            calibEdgePoint1 = nullptr;
            calibStepPoint1 = Icons::StepState::Active;
            calibPickInvalidated = true;
        }
        if (calibFacePoint2 != nullptr && !stillPickable(calibFacePoint2))
        {
            calibFacePoint2 = nullptr;
            calibStepPoint2 = Icons::StepState::Active;
            calibPickInvalidated = true;
        }
        if (calibEdgePoint2 != nullptr && !stillPickableEdge(calibEdgePoint2))
        {
            calibEdgePoint2 = nullptr;
            calibStepPoint2 = Icons::StepState::Active;
            calibPickInvalidated = true;
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

        if (hoverPickFace != nullptr || hoverPickEdge != nullptr || calibFacePoint1 != nullptr ||
            calibFacePoint2 != nullptr || calibEdgePoint1 != nullptr || calibEdgePoint2 != nullptr)
            RebuildPickHighlightMesh();

        if (analysisEnabled)
        {
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

            // Overhang face bounds
            for (const Face *face : overhangFaces)
                expandFaceBounds(overhangMin, overhangMax, face);

            // Thin section / small feature bounds from faceFlawRanges
            for (const auto &[solid, flaws] : results.faceFlawRanges)
            {
                for (const auto &ff : flaws)
                {
                    if (ff.flaw == FaceFlawKind::THIN_SECTION && ff.face)
                        expandFaceBounds(thinMin, thinMax, ff.face);
                    else if (ff.flaw == FaceFlawKind::SMALL_FEATURE && ff.face)
                        expandFaceBounds(smallMin, smallMax, ff.face);
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

            auto makeFrameCallback = [this](glm::vec3 bMin, glm::vec3 bMax) -> std::function<void()>
            {
                if (bMin.x > bMax.x)
                    return nullptr; // no valid bounds
                return [this, bMin, bMax]()
                {
                    camera.FrameBounds(bMin, bMax);
                    cameraDirty = true;
                    renderDirty = true;
                };
            };

            // Write live flaw state — read each frame by the imguiContent lambdas in uiResult
            flawOverhang.count = overhangs;
            flawOverhang.frameCallback = makeFrameCallback(overhangMin, overhangMax);
            flawSharp.count = sharpEdges;
            flawSharp.frameCallback = makeFrameCallback(sharpMin, sharpMax);
            flawThin.count = thinSections;
            flawThin.frameCallback = makeFrameCallback(thinMin, thinMax);
            flawSmall.count = smallFeatures;
            flawSmall.frameCallback = makeFrameCallback(smallMin, smallMax);

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
        }
        else
        {
            lastVerdictWasPass = false;
            flawOverhang = {};
            flawSharp = {};
            flawThin = {};
            flawSmall = {};
            if (uiVerdict)
                uiVerdict->values = {};
        }
        sceneDirty = false;
    }

    if (cameraMovedForPick)
    {
        float mx, my;
        SDL_GetMouseState(&mx, &my);
        UpdatePickHover(mx, my);
    }

    if (!statusStripImportBusy)
        RefreshStatusStripIdleText();

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

    camera.SetAspectRatio(static_cast<float>(width) / static_cast<float>(height));
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
        calibPara_Point2 && calibPara_Point2->selected && calibStepPoint1 == Icons::StepState::Done;
    if (!awaitingPoint1Pick && !awaitingPoint2Pick)
        return PickFilter::None;

    return PickFilter::Faces;
}

void Display::ClearPickHover()
{
    hoverPickFace = nullptr;
    hoverPickEdge = nullptr;
    RebuildPickHighlightMesh();
    renderDirty = true;
}

void Display::ClearCalibrateFacePicks()
{
    calibFacePoint1 = nullptr;
    calibFacePoint2 = nullptr;
    calibEdgePoint1 = nullptr;
    calibEdgePoint2 = nullptr;
    RefreshCalibWorkflow();
    RebuildPickHighlightMesh();
    renderDirty = true;
}

void Display::SetHoverCalibPick(const Face *face, const Edge *edge)
{
    if (hoverPickFace == face && hoverPickEdge == edge)
    {
        if (face != nullptr || edge != nullptr)
            return;
        if (calibFacePoint1 != nullptr || calibFacePoint2 != nullptr || calibEdgePoint1 != nullptr ||
            calibEdgePoint2 != nullptr)
            return;
        if (pickHighlightIndices.empty())
            return;
    }
    hoverPickFace = face;
    hoverPickEdge = edge;
    RebuildPickHighlightMesh();
    renderDirty = true;
}

void Display::RebuildPickHighlightMesh()
{
    pickHighlightVertices.clear();
    pickHighlightIndices.clear();
    pickHighlightLineVertices.clear();
    pickHighlightLineIndices.clear();

    const std::vector<PickTriangle> &tris = renderer.GetPickTriangles();
    uint32_t nextVert = 0;

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

    appendFaceTris(calibFacePoint1, 1.0f, 0.72f);
    appendFaceTris(calibFacePoint2, 1.0f, 0.72f);

    const std::vector<PickSegment> &segPick = renderer.GetPickSegments();
    const glm::vec3 lineNormal(0.0f, 0.0f, 1.0f);
    auto appendEdgeLines = [&](const Edge *edge, float accentDepthSteps, float satMult)
    {
        if (edge == nullptr)
            return;
        const glm::vec3 accent = glm::vec3(Color::GetAccentSteps(accentDepthSteps, 1.0f, satMult));
        for (const PickSegment &ps : segPick)
        {
            if (ps.edge != edge)
                continue;
            const uint32_t base = static_cast<uint32_t>(pickHighlightLineVertices.size());
            pickHighlightLineVertices.push_back({glm::vec3(ps.v0), accent, lineNormal});
            pickHighlightLineVertices.push_back({glm::vec3(ps.v1), accent, lineNormal});
            pickHighlightLineIndices.push_back(base);
            pickHighlightLineIndices.push_back(base + 1);
        }
    };

    appendEdgeLines(calibEdgePoint1, 1.0f, 0.72f);
    appendEdgeLines(calibEdgePoint2, 1.0f, 0.72f);

    const Face *hoverDraw = hoverPickFace;
    if (hoverDraw == calibFacePoint1 || hoverDraw == calibFacePoint2)
        hoverDraw = nullptr;
    appendFaceTris(hoverDraw, 0.5f, 0.5f);

    const Edge *hoverEdgeDraw = hoverPickEdge;
    if (hoverEdgeDraw == calibEdgePoint1 || hoverEdgeDraw == calibEdgePoint2)
        hoverEdgeDraw = nullptr;
    appendEdgeLines(hoverEdgeDraw, 0.5f, 0.5f);

    renderer.UploadPickHighlightMesh(pickHighlightVertices, pickHighlightIndices);
    renderer.UploadPickHighlightLineMesh(pickHighlightLineVertices, pickHighlightLineIndices);
}

Display::CalibPickHit Display::PickCalibrateAtPixel(float pixelX, float pixelY) const
{
    CalibPickHit out;
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    glm::dvec3 ro, rd;
    ScenePick::OrthoPickRay(camera, w, h, pixelX, pixelY, ro, rd);

    double faceT = 0.0;
    const Face *face = ScenePick::PickClosestFace(renderer.GetPickTriangles(), ro, rd, PickFilter::Faces, &faceT);

    const double halfH = static_cast<double>(camera.orthoSize);
    const double worldPerPx =
        std::max(2.0 * halfH / std::max(1, h), 2.0 * halfH * static_cast<double>(camera.aspectRatio) / std::max(1, w));
    const double tol = 10.0 * worldPerPx;
    const double maxDistSq = tol * tol;

    double edgeT = 0.0;
    double edgeDistSq = 0.0;
    const Edge *edge = ScenePick::PickClosestEdgeAlongRay(renderer.GetPickSegments(), ro, rd, maxDistSq, &edgeT,
                                                           &edgeDistSq);

    if (face == nullptr && edge == nullptr)
        return out;

    if (face == nullptr)
    {
        out.edge = edge;
        return out;
    }
    if (edge == nullptr)
    {
        out.face = face;
        return out;
    }

    constexpr double kRayEps = 1e-4;
    constexpr double kTightEdgeFrac = 0.04;
    if (edgeDistSq < maxDistSq * kTightEdgeFrac && edgeT < faceT + kRayEps * (1.0 + std::abs(faceT)))
    {
        out.edge = edge;
        return out;
    }
    if (edgeT + kRayEps < faceT)
    {
        out.edge = edge;
        return out;
    }
    out.face = face;
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
    SetHoverCalibPick(hit.face, hit.edge);
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
    if (hit.face == nullptr && hit.edge == nullptr)
        return;

    if (calibPara_Point1->selected)
    {
        calibFacePoint1 = hit.face;
        calibEdgePoint1 = hit.edge;
        calibStepPoint1 = Icons::StepState::Done;
        calibPara_Point1->selected = false;
        calibPara_Point2->selected = true;
    }
    else if (calibPara_Point2 && calibPara_Point2->selected)
    {
        if (calibStepPoint1 != Icons::StepState::Done)
            return;
        calibFacePoint2 = hit.face;
        calibEdgePoint2 = hit.edge;
        calibStepPoint2 = Icons::StepState::Done;
        calibPara_Point2->selected = false;
    }
    else
        return;

    RefreshCalibWorkflow();
    uiRenderer.MarkDirty();
    RebuildPickHighlightMesh();
    renderDirty = true;
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
    std::unordered_set<const Edge *> holeEdges;
    CalibrateDistance::RebuildHoleEdgeSet(*scene, holeEdges);
    const double layerMm = static_cast<double>(layerHeight);
    const Face *r1 = ResolveCalibFaceForWorkflow(calibFacePoint1, calibEdgePoint1);
    const Face *r2 = ResolveCalibFaceForWorkflow(calibFacePoint2, calibEdgePoint2);
    if (r1 != nullptr && r2 != nullptr)
        calibWorkflow = CalibrateDistance::CombinePickedFaces(r1, r2, scene, layerMm, holeEdges);
    else if (r1 != nullptr)
        calibWorkflow = CalibrateDistance::ClassifyFace(r1, scene, layerMm, holeEdges);
    else if (r2 != nullptr)
        calibWorkflow = CalibrateDistance::ClassifyFace(r2, scene, layerMm, holeEdges);
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
    if (calibSec_Parameters && calibSec_Parameters->visible)
    {
        const bool importDone = calibStepImport == Icons::StepState::Done;
        const bool firstFaceDone = calibStepPoint1 == Icons::StepState::Done;
        next = importDone && firstFaceDone;
    }

    if (calibPara_Derived->visible != next)
    {
        calibPara_Derived->visible = next;
        uiRenderer.MarkDirty();
    }
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

    const CalibrateNominal::SpanResult span = CalibrateNominal::SpanBetweenFaces(spanA, spanB);
    if (!span.valid)
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
    if (std::abs(x) <= std::abs(y) * 0.5f)
        x = 0;
    else if (std::abs(y) <= std::abs(x) * 0.5f)
        y = 0;
}

void Display::Orbit(float offsetX, float offsetY)
{
    camera.Orbit(offsetX, offsetY);

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

    s.activeToolOrdinal = (activeTool == ActiveTool::Calibrate) ? 1u : 0u;
    s.viewportAnalysisEnabled = analysisEnabled;
    s.depthExperimentOrdinal = static_cast<uint8_t>(ViewportDepthExperiments::Active());
}

void Display::SyncToolbarToolVisualState()
{
    if (toolbarAnalysisLine && toolbarCalibrateLine && uiAnalysis && uiCalibrate)
    {
        toolbarAnalysisLine->selected =
            (activeTool == ActiveTool::Analysis && uiAnalysis->visible);
        toolbarCalibrateLine->selected =
            (activeTool == ActiveTool::Calibrate && uiCalibrate->visible);
    }
    uiRenderer.MarkDirty();
}

void Display::SyncStatusStripTextLine()
{
    if (statusStripTextLine != nullptr)
        statusStripTextLine->text = statusStripLine;
    uiRenderer.MarkDirty();
}

void Display::RefreshStatusStripIdleText()
{
    if (statusStripImportBusy)
    {
        if (uiStatusStrip)
        {
            uiStatusStrip->visible = true;
            SyncStatusStripTextLine();
        }
        return;
    }
    if (scene == nullptr)
    {
        statusStripLine.clear();
        if (statusStripTextLine != nullptr)
            statusStripTextLine->text.clear();
        if (uiStatusStrip)
        {
            uiStatusStrip->visible = false;
            uiRenderer.MarkDirty();
        }
        return;
    }
    const size_t nSol = scene->solids.size();
    const unsigned renderedVerts = static_cast<unsigned>(renderer.UploadedTriangleVertexCount() +
                                                          renderer.UploadedLineVertexCount());
    char buf[96];
    if (nSol == 1)
        snprintf(buf, sizeof(buf), "1 solid, %u rendered vertices", renderedVerts);
    else
        snprintf(buf, sizeof(buf), "%zu solids, %u rendered vertices", nSol, renderedVerts);
    statusStripLine.assign(buf);
    if (uiStatusStrip)
        uiStatusStrip->visible = true;
    SyncStatusStripTextLine();
}

void Display::CompleteFileImport(const std::string &path)
{
    auto ext = path.substr(path.find_last_of('.') + 1);
    std::string lower;
    for (char c : ext)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Each import gets its own independent scene.
    ownedScenes.push_back(std::make_unique<Scene>());
    activeSceneIndex = ownedScenes.size() - 1;
    scene = ownedScenes.back().get();

    if (lower == "stl")
        STLImport::Import(path, scene);
    else if (lower == "obj")
        OBJImport::Import(path, scene);
    else if (lower == "3mf")
        ThreeMFImport::Import(path, scene);

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
    }

    openFiles.push_back(filename);
    RebuildFileTabs();

    calibStepImport = Icons::StepState::Done;
    ClearCalibrateFacePicks();
    calibPara_Import->visible = false;
    calibPara_Point1->visible = true;
    calibPara_Point1->selected = true;
    calibStepPoint1 = Icons::StepState::Active;
    calibPara_Point2->visible = true;
    calibStepPoint2 = Icons::StepState::Active;
    if (calibSec_Parameters)
        calibSec_Parameters->visible = true;
    RefreshCalibDerivedRowVisible();
    uiRenderer.MarkDirty();
    renderDirty = true;

    statusStripImportBusy = false;
    statusStripImportProgress01 = -1.0f;
    RefreshStatusStripIdleText();
}

void Display::ProcessDeferredImportIfAny()
{
    if (!deferredImportPath)
        return;
    std::string path = std::move(*deferredImportPath);
    deferredImportPath.reset();

    const std::string fname = std::filesystem::path(path).filename().string();
    statusStripImportBusy = true;
    statusStripImportProgress01 = -1.0f;
    statusStripLine = "Importing " + fname + "…";
    if (uiStatusStrip)
        uiStatusStrip->visible = true;
    SyncStatusStripTextLine();
    Render();

    CompleteFileImport(path);
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

    uiFiles->children.clear();
    uiFiles->children.reserve(openFiles.size() + 1); // file tabs + "+" button

    for (size_t i = 0; i < openFiles.size(); i++)
    {
        // Use "file_N" as the paragraph id — unique even if two files share the same name.
        // Visible label comes from line.text, not the id.
        Paragraph &tab = uiFiles->AddParagraph("file_" + std::to_string(i));
        tab.values.reserve(1);
        SectionLine &line = tab.values.emplace_back();
        line.text = std::filesystem::path(openFiles[i]).stem().string();
        line.selected = (i == activeSceneIndex);
        line.onClick = [this, i]()
        {
            ClearPickHover();
            ClearCalibrateFacePicks();
            calibStepPoint1 = Icons::StepState::Active;
            calibStepPoint2 = Icons::StepState::Active;
            if (calibPara_Point1)
                calibPara_Point1->selected = true;
            if (calibPara_Point2)
                calibPara_Point2->selected = false;

            scene = ownedScenes[i].get();
            activeSceneIndex = i;
            UpdateScene();
            FrameScene();
            pendingFileTabsRebuild = true;
            uiRenderer.MarkDirty();
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
        uiToolbar->children.reserve(2);

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
                    SyncToolbarToolVisualState();
                    renderDirty = true;
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
                    SyncToolbarToolVisualState();
                    renderDirty = true;
                    return;
                }
                activeTool = ActiveTool::Calibrate;
                pendingToolSwitch = true;
                renderDirty = true;
            };
            toolbarCalibrateLine = &line;
        }
    }

    // Status strip — same root-panel stack as Settings/Toolbar (GL card + ImGui row). Layout pass in
    // UIRenderer::ResolveAnchors shifts Settings/Toolbar down by this panel's row span.
    {
        RootPanel statusDef;
        statusDef.id = "StatusStrip";
        statusDef.bgParentDepth = 0;
        statusDef.leftAnchor = PanelAnchor{uiSettings, PanelAnchor::Left};
        statusDef.rightAnchor = PanelAnchor{uiToolbar, PanelAnchor::Right};
        statusDef.topAnchor = PanelAnchor{nullptr, PanelAnchor::Top};
        // Match Files / other root panels: default RootPanel borderRadius + margin + padding;
        // default Paragraph (layer 2) margin/padding — same box model as header/body rows elsewhere.
        uiStatusStrip = &uiRenderer.AddPanel(statusDef);
        uiStatusStrip->children.reserve(1); // RootPanel::AddParagraph requires capacity > size (see Panel.hpp)
        Paragraph &stripPara = uiStatusStrip->AddParagraph("Line");
        SectionLine &stripLine = stripPara.values.emplace_back();
        stripLine.fontScale = 1.0f;
        stripLine.textDepth = 2;
        statusStripTextLine = &stripLine;
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
    uiAnalysis->children.reserve(4); // stable pointers: Result + ImportAction + Verdict + Configs
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
                           bool isAngle               // true = format "%.0f", false = "%.2f"/"%.1f" based on dragSpeed
                       )
    {
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
        line.getMinContentWidthPx = [this, countLabel, plural, minWidthLabel, unit, isAngle]() -> float
        {
            ImFont *f = uiRenderer.GetPixelImFont();
            if (!f)
                return 0.0f;
            float pad = ImGui::GetStyle().FramePadding.x;
            std::string longestCount = std::string("No") + countLabel + plural;
            float countW = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, longestCount.c_str()).x;
            std::string longestVal = isAngle ? std::string(minWidthLabel) + unit
                                             : std::string(minWidthLabel) + " " + unit;
            float valW = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, longestVal.c_str()).x;
            constexpr float gap = 24.0f;
            return pad * 2.0f + countW + gap + valW;
        };

        line.imguiContent = [this, flawColor, countLabel, plural, flawMember,
                             &param, dragSpeed, dragMin, dragMax,
                             unit, dragId, isAngle](float w, float h, float iconOffset)
        {
            FlawResult &fr = this->*flawMember;
            glm::vec4 dimColor = Color::GetUIText(1);
            glm::vec4 dimLow = Color::GetUIText(0);

            UIStyle::PushInputStyle(h, dimColor);
            float normalPad = ImGui::GetStyle().FramePadding.x;
            ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
            float originX = rowOrigin.x;

            // ── Left nav zone: InvisibleButton placed BEFORE DragFloat ──────────
            // Compute left zone width from count label (CalcTextSize valid here — inside a frame)
            char countBuf[64];
            snprintf(countBuf, sizeof(countBuf), "%zu%s%s", fr.count, countLabel,
                     fr.count > 1 ? plural : "");
            float leftW = iconOffset + ImGui::CalcTextSize(countBuf).x + normalPad * 2.5f;
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
            ImGui::SetCursorScreenPos(ImVec2(originX + leftW + editGap, rowOrigin.y));

            // ── Right param zone: DragFloat ──────────────────────────────────────
            float rightW = w - leftW - editGap;

            if (fr.requestEdit)
            {
                ImGui::SetKeyboardFocusHere();
                fr.requestEdit = false;
                fr.focusPending = true;
                showEdit = true;
            }

            ImGui::SetNextItemWidth(rightW);
            const char *fmt = showEdit ? (isAngle ? "%.0f" : (dragSpeed < 0.1f ? "%.2f" : "%.1f")) : "";
            bool changed = ImGui::DragFloat(dragId, &param, dragSpeed, dragMin, dragMax, fmt);
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

            // ── Text overlay ─────────────────────────────────────────────────────
            float ty = ImGui::GetItemRectMin().y + ImGui::GetStyle().FramePadding.y;
            ImU32 flawCol = ImGui::GetColorU32(ImVec4(flawColor.r, flawColor.g, flawColor.b, flawColor.a));
            ImU32 dimCol = ImGui::GetColorU32(ImVec4(dimLow.r, dimLow.g, dimLow.b, dimLow.a));
            ImU32 dimColZero = ImGui::GetColorU32(ImVec4(dimLow.r, dimLow.g, dimLow.b, dimLow.a * 0.5f));

            // Left label — always visible (even during text edit)
            if (fr.count > 0)
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(originX + iconOffset + normalPad, ty), flawCol, countBuf);
            else
            {
                std::string noFlawLabel = std::string("No") + countLabel + plural;
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(originX + iconOffset + normalPad, ty), dimColZero, noFlawLabel.c_str());
            }

            // Right side: param value in normal mode, unit hint in edit mode
            if (!showEdit)
            {
                char valBuf[32];
                if (isAngle)
                    snprintf(valBuf, sizeof(valBuf), "%.0f%s", param, unit);
                else if (dragSpeed < 0.1f)
                    snprintf(valBuf, sizeof(valBuf), "%.2f %s", param, unit);
                else
                    snprintf(valBuf, sizeof(valBuf), "%.1f %s", param, unit);
                ImVec2 vs = ImGui::CalcTextSize(valBuf);
                ImU32 valCol = (fr.count > 0) ? dimCol : dimColZero;
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(originX + w - normalPad - vs.x, ty), valCol, valBuf);
            }
            else
            {
                ImVec2 us = ImGui::CalcTextSize(unit);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(originX + w - normalPad - us.x, ty), dimCol, unit);
            }

            if (navFired)
                fr.frameCallback();
            if (changed)
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
                overhangAngle, 0.5f, 0.0f, 90.0f, "\u00b0", "##overhang", "90", true);

    makeFlawRow(uiResult->values.emplace_back(), edgeColor,
                Icons::SharpCorner(edgeColor),
                " sharp edge", "s", &Display::flawSharp,
                sharpCornerAngle, 0.5f, 0.0f, 180.0f, "\u00b0", "##sharp", "180", true);

    makeFlawRow(uiResult->values.emplace_back(), thinColor,
                Icons::ThinSection(thinColor),
                " thin section", "s", &Display::flawThin,
                thinMinWidth, 0.05f, 0.1f, 50.0f, "mm", "##thinsection", "2.0", false);

    makeFlawRow(uiResult->values.emplace_back(), smallColor,
                Icons::SmallFeature(smallColor),
                " small feature", "s", &Display::flawSmall,
                minFeatureSize, 0.05f, 0.1f, 50.0f, "mm", "##smallfeature", "10.0", false);

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
        line.getMinContentWidthPx = [settingsBodyFont, label]() -> float
        {
            if (!settingsBodyFont)
                return 0.0f;
            float pad = ImGui::GetStyle().FramePadding.x;
            float labelW = settingsBodyFont->CalcTextSizeA(settingsBodyFont->FontSize, FLT_MAX, 0.0f, label).x;
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

            float labelFontSz = settingsBodyFont ? settingsBodyFont->FontSize : ImGui::GetFont()->FontSize;

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
                float labelTextW = settingsBodyFont
                                       ? settingsBodyFont->CalcTextSizeA(settingsBodyFont->FontSize, FLT_MAX, 0.0f, label).x
                                       : ImGui::CalcTextSize(label).x;
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
                float ty_label = bottom - labelFontSz;
                if (settingsBodyFont)
                {
                    ImGui::PushFont(settingsBodyFont);
                    dl->AddText(ImVec2(originX + iconOffset + pad, ty_label), labelCol, label);
                    ImGui::PopFont();
                }
                else
                    dl->AddText(ImVec2(originX + iconOffset + pad, ty_label), labelCol, label);
            }

            // Value overlay: right edge, hidden during text edit (DragFloat renders it).
            if (!showEdit)
            {
                char valBuf[32];
                snprintf(valBuf, sizeof(valBuf), valueFmt, param);
                ImVec2 vs = ImGui::CalcTextSize(valBuf);
                float ty_value = bottom - ImGui::GetFont()->FontSize;
                ImU32 valCol = ImGui::GetColorU32(ImVec4(tcValue.r, tcValue.g, tcValue.b, tcValue.a));
                dl->AddText(ImVec2(originX + w - pad - vs.x, ty_value), valCol, valBuf);
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
    appearancePara.values.reserve(2);

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
                    renderDirty = true;
                }
            }
            else
            {
                Color::SetAccent(settingsAccentHue, settingsAccentSat);
                uiRenderer.MarkDirty();
                renderDirty = true;
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
                    renderDirty = true;
                }
                ImGui::EndPopup();
            }
        };
        accentSel.select = std::move(sel);
        uiAppearanceAccentSelect = &accentSel.select.value();
    }
    // ── Viewport ──────────────────────────────────────────────────────────────
    Section &viewportSection = uiSettings->AddSection("Viewport");
    viewportSection.header = Header{"Viewport", 1.0f, 2};
    viewportSection.tightHeader = true;
    viewportSection.children.reserve(1);

    Paragraph &gridPara = viewportSection.AddParagraph("GridSize");
    gridPara.values.reserve(1);
    makeSettingsDrag(gridPara.values.emplace_back(), "Grid size", Color::GRID_EXTENT,
                     4.0f, 16.0f, 2048.0f, "%.0f", "##gridExtent",
                     [this]()
                     {
                         lastSyncedAxisWorldHalfExtent = std::numeric_limits<float>::quiet_NaN();
                         (void)SyncViewportAxisForDepthClip();
                         renderDirty = true;
                     });

    // ── Navigation ────────────────────────────────────────────────────────────
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

    // ── Calibrate panel ───────────────────────────────────────────────────────
    {
        ToolPanelDef calibDef;
        calibDef.id = "Calibrate";
        calibDef.name = "Calibrate";
        calibDef.description = "Calibrate your 3d printer through measurements";

        // ── Prerequisites ──────────────────────────────────────────────────
        calibDef.prerequisites.reserve(3);
        calibDef.prerequisites.push_back({"CalibImport", "Import a file", "",
                                          Icons::CheckBox(&calibStepImport), false, true,
                                          [this]()
                                          { DoFileImport(); }});
        calibDef.prerequisites.push_back({"CalibPoint1", "Plot measurement point",
                                          "to calibrate against",
                                          Icons::CheckBox(&calibStepPoint1), false, false});
        calibDef.prerequisites.push_back({"CalibPoint2", "Plot measurement point", "",
                                          Icons::CheckBox(&calibStepPoint2), false, false});

        // ── Parameters — print measurement InputFloat ──────────────────────
        // TODO(Calibrate): Inline CAD nominal span (and pick/span status when needed) on this row so
        // CalibDerived can stay compensation-only; see CalibDerived for messages still on the row below.
        {
            ParameterDef pm;
            pm.id = "CalibMeasure";
            pm.line.iconDraw = Icons::StepDot(&calibStepMeasure);
            pm.line.getMinContentWidthPx = [settingsBodyFont]() -> float
            {
                if (!settingsBodyFont)
                    return 0.0f;
                float pad = ImGui::GetStyle().FramePadding.x;
                float labelW = settingsBodyFont->CalcTextSizeA(settingsBodyFont->FontSize, FLT_MAX, 0.0f, "Print measurement").x;
                return pad * 2.0f + labelW + 24.0f + 48.0f;
            };
            auto pmEditing = std::make_shared<bool>(false);
            pm.line.imguiContent = [this, settingsBodyFont, pmEditing](float w, float h, float iconOffset)
            {
                glm::vec4 tcLabel = Color::GetUIText(2);
                glm::vec4 tcValue = Color::GetUIText(0);
                float pad = ImGui::GetStyle().FramePadding.x;

                UIStyle::PushInputStyle(h, tcLabel);
                ImVec2 rowOrigin = ImGui::GetCursorScreenPos();

                float labelTextW = settingsBodyFont
                                       ? settingsBodyFont->CalcTextSizeA(settingsBodyFont->FontSize, FLT_MAX, 0.0f, "Print measurement").x
                                       : ImGui::CalcTextSize("Print measurement").x;
                // Keep the input visually closer to the label (was too far right).
                float leftW = pad + labelTextW + pad * 1.25f;
                float inputW = w - leftW;

                ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x + leftW, rowOrigin.y));
                ImGui::SetNextItemWidth(inputW);
                bool showEdit = *pmEditing;
                if (!showEdit)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
                bool changed = ImGui::InputFloat("##calibMeasured", &calibMeasured, 0.0f, 0.0f, "%.2f");
                if (!showEdit)
                    ImGui::PopStyleColor();
                UIStyle::DrawInputHoverTint(1);
                *pmEditing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;

                ImDrawList *dl = ImGui::GetWindowDrawList();
                float itemBottom = ImGui::GetItemRectMax().y;
                float labelTextH = settingsBodyFont
                                       ? settingsBodyFont->CalcTextSizeA(settingsBodyFont->FontSize, FLT_MAX, 0.0f, "Print measurement").y
                                       : ImGui::CalcTextSize("Print measurement").y;
                ImU32 labelCol = ImGui::GetColorU32(ImVec4(tcLabel.r, tcLabel.g, tcLabel.b, tcLabel.a));
                if (settingsBodyFont)
                    ImGui::PushFont(settingsBodyFont);
                dl->AddText(ImVec2(rowOrigin.x + pad, itemBottom - labelTextH), labelCol, "Print measurement");
                if (settingsBodyFont)
                    ImGui::PopFont();

                // Unit/value suffix — value is right-aligned and sits directly left of unit.
                {
                    const char *unit = "mm";
                    ImVec2 us = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFont()->FontSize, FLT_MAX, 0.0f, unit);
                    ImU32 unitCol = ImGui::GetColorU32(ImVec4(tcValue.r, tcValue.g, tcValue.b, tcValue.a));
                    dl->AddText(ImVec2(rowOrigin.x + w - pad - us.x, itemBottom - us.y), unitCol, unit);

                    if (!*pmEditing)
                    {
                        char valueBuf[32];
                        std::snprintf(valueBuf, sizeof(valueBuf), "%.2f", calibMeasured);
                        ImVec2 vs = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFont()->FontSize, FLT_MAX, 0.0f, valueBuf);
                        constexpr float valueUnitGap = 10.0f;
                        dl->AddText(ImVec2(rowOrigin.x + w - pad - us.x - valueUnitGap - vs.x, itemBottom - vs.y), unitCol, valueBuf);
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

        {
            ParameterDef pmDer;
            pmDer.id = "CalibDerived";
            pmDer.line.iconDraw = Icons::StepDot(&calibStepMeasure);
            pmDer.line.getMinContentWidthPx = [settingsBodyFont]() -> float
            {
                if (!settingsBodyFont)
                    return 0.0f;
                float pad = ImGui::GetStyle().FramePadding.x;
                float labelW = settingsBodyFont->CalcTextSizeA(settingsBodyFont->FontSize, FLT_MAX, 0.0f,
                                                                "First-layer excess (printed − CAD)").x;
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

                auto drawLine = [&](float y, const char *label, const char *valueStr)
                {
                    ImU32 lc = ImGui::GetColorU32(ImVec4(tcLabel.r, tcLabel.g, tcLabel.b, tcLabel.a));
                    ImU32 vc = ImGui::GetColorU32(ImVec4(tcValue.r, tcValue.g, tcValue.b, tcValue.a));
                    if (settingsBodyFont)
                        ImGui::PushFont(settingsBodyFont);
                    ImVec2 ls = ImGui::CalcTextSize(label);
                    dl->AddText(ImVec2(row0.x + pad, y), lc, label);
                    ImVec2 vs = ImGui::CalcTextSize(valueStr);
                    dl->AddText(ImVec2(row0.x + w - pad - vs.x, y), vc, valueStr);
                    if (settingsBodyFont)
                        ImGui::PopFont();
                };

                float lh = settingsBodyFont
                               ? settingsBodyFont->CalcTextSizeA(settingsBodyFont->FontSize, FLT_MAX, 0.0f, "Mg").y
                               : ImGui::GetTextLineHeight();
                const float y0 = row0.y + pad * 0.25f;

                const bool missingFaces =
                    !CalibSlotHasPick(calibFacePoint1, calibEdgePoint1) ||
                    !CalibSlotHasPick(calibFacePoint2, calibEdgePoint2);
                const bool spanBad = !missingFaces && calibNominal <= 1e-5f;
                const bool importDone = calibStepImport == Icons::StepState::Done;
                const bool firstFaceDone = calibStepPoint1 == Icons::StepState::Done;

                if (missingFaces)
                {
                    // No span text until import and first face pick are done — avoids "pick two faces" while
                    // prerequisite rows still describe the first measurement point.
                    if (!importDone || !firstFaceDone)
                    {
                        ImGui::Dummy(ImVec2(w, pad));
                        return;
                    }
                    drawLine(y0, "Pick second face or edge for CAD span.", "");
                    ImGui::Dummy(ImVec2(w, lh * 1.4f + pad));
                    return;
                }
                if (spanBad)
                {
                    drawLine(y0, "Could not estimate span (try parallel faces).", "");
                    ImGui::Dummy(ImVec2(w, lh * 1.4f + pad));
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
                        lab = "shrinkage";
                        std::snprintf(valB, sizeof(valB), "%.4f", calibContourScale);
                        break;
                    case CalibWorkflow::Hole:
                        lab = "Hole radius offset";
                        std::snprintf(valB, sizeof(valB), "%.3f mm", calibHoleOffsetMm);
                        break;
                    case CalibWorkflow::ElephantFoot:
                        lab = "First-layer excess (printed − CAD)";
                        std::snprintf(valB, sizeof(valB), "%.3f mm", calibElephantFootMm);
                        break;
                    default:
                        break;
                    }
                    if (lab[0] != '\0')
                        drawLine(y, lab, valB);
                }
                else
                {
                    drawLine(y, "Adjust print measurement to compute compensation.", "");
                }

                ImGui::Dummy(ImVec2(w, lh * 1.4f + pad));
            };
            calibDef.parameters.push_back(std::move(pmDer));
        }

        RootPanel calibPanel = BuildToolPanel(calibDef);
        calibPanel.visible = false;
        calibPanel.leftAnchor = PanelAnchor{uiToolbar, PanelAnchor::Right};
        calibPanel.topAnchor = PanelAnchor{uiFiles, PanelAnchor::Bottom};
        uiCalibrate = &uiRenderer.AddPanel(calibPanel);

        // ── Paragraph pointers for live state mutation ──────────────────────
        Section *prereqs = FindSection(*uiCalibrate, "Prerequisites");
        calibPara_Import = &prereqs->children[0];
        calibPara_Point1 = &prereqs->children[1];
        calibPara_Point2 = &prereqs->children[2];
        calibLine_Point1Primary = &prereqs->children[1].values[0];
        calibLine_Point2Primary = &prereqs->children[2].values[0];

        calibSec_Parameters = FindSection(*uiCalibrate, "Parameters");
        if (calibSec_Parameters && calibSec_Parameters->children.size() >= 2)
            calibPara_Derived = &calibSec_Parameters->children[1];

        // Click handlers — selecting a point prerequisite deselects the other.
        auto selectPoint1 = [this]()
        {
            calibPara_Point1->selected = true;
            calibPara_Point2->selected = false;
        };
        auto selectPoint2 = [this]()
        {
            calibPara_Point2->selected = true;
            calibPara_Point1->selected = false;
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

        RefreshCalibDerivedRowVisible();
    }

    // Compute minimum grid extent and enforce as SDL minimum window size
    uiRenderer.ComputeMinGridSize();
    const auto &grid = uiRenderer.GetGrid();
    SDL_SetWindowMinimumSize(window, grid.MinWidthPixels(), grid.MinHeightPixels());
}