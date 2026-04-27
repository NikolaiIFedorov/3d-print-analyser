#pragma once

#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <limits>
#include <memory>
#include <string>
#include "utils/utils.hpp"
#include "rendering/SceneRenderer/SceneRenderer.hpp"
#include "rendering/ViewportRenderer/ViewportRenderer.hpp"
#include "rendering/UIRenderer/UIRenderer.hpp"
#include "rendering/UIRenderer/Icons.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include "utils/Settings.hpp"
#include "utils/SessionLogger.hpp"
#include "rendering/ScenePick.hpp"
#include "Geometry/Geometry.hpp"
#include "Calibrate/CalibDistanceType.hpp"

struct Edge;

class Display
{
public:
    Display(int16_t width, int16_t height, const char *title);
    void Shutdown();

    SDL_Window *GetWindow() { return window; }
    SceneRenderer *GetRenderer() { return &renderer; }
    UIRenderer *GetUIRenderer() { return &uiRenderer; }

    void Render();
    void UpdateScene();
    void Frame();

    bool HitTestUI(float pixelX, float pixelY) const;
    /// True if (pixelX, pixelY) lies over a Dear ImGui window that accepts mouse input (uses last frame rects).
    bool HitTestImGui(float pixelX, float pixelY) const;

    void SetAspectRatio(uint16_t width, uint16_t height);

    void UpdateCamera();

    static bool ResizeEventWatcher(void *userdata, SDL_Event *event);
    Camera GetCamera() { return camera; }
    void Zoom(const float offsetY, const glm::vec3 &posCursotr);
    void Orbit(float offsetX, float offsetY);
    void Roll(float delta);
    void Pan(float offsetX, float offsetY, bool scroll = true);
    void FrameScene();
    void ResetCameraView();
    glm::vec3 ScreenToWorld(float pixelX, float pixelY) const;
    void MarkBug();

    /// Fills `out` with scene counts, analysis params, camera pose, clip planes, window sizes, and tool flags for logging.
    void FillSessionReproState(SessionState &out) const;

    /// Face/edge hover for tools that use `GetActivePickFilter()` (e.g. Calibrate). Respects UI hit-test and ImGui capture.
    void UpdatePickHover(float pixelX, float pixelY);
    /// Left-click in viewport: commits hovered face or edge to the active Calibrate point prerequisite when applicable.
    void TryCommitCalibrateFacePick(float pixelX, float pixelY);
    PickFilter GetActivePickFilter() const;

    bool renderDirty = true;
    float mouseSensitivity = 30.0f; // display units: raw value × 10000 (1–500); Input.cpp compensates

    Settings settings;

    enum class ThemeMode
    {
        System,
        Light,
        Dark
    };
    // themeMode mirrors settings.themeMode as a typed enum.
    ThemeMode themeMode = ThemeMode::System;

    enum class ActiveTool
    {
        Analysis,
        Calibrate
    };
    ActiveTool activeTool = ActiveTool::Analysis;

    // Apply the resolved appearance (dark/light) from themeMode + current system theme.
    // Call at startup, on ThemeMode change, and on SDL_EVENT_SYSTEM_THEME_CHANGED.
    void ApplyTheme();

private:
    int16_t windowWidth;
    int16_t windowHeight;
    SDL_Window *InitWindow(int16_t width, int16_t height, const char *title);
    SDL_Window *window = nullptr;
    SDL_GLContext glContext = nullptr;

    SceneRenderer renderer;
    ViewportRenderer viewportRenderer;
    UIRenderer uiRenderer;
    Scene baseScene;                                 // empty base — active when no file is loaded
    std::vector<std::unique_ptr<Scene>> ownedScenes; // one per imported file
    size_t activeSceneIndex = SIZE_MAX;              // SIZE_MAX = base scene active
    Scene *scene = nullptr;                          // pointer to active scene

    Camera camera;

    bool analysisEnabled = true;
    bool cameraDirty = true;
    bool sceneDirty = true;

    float overhangAngle = 45.0f;
    float sharpCornerAngle = 100.0f;
    float minFeatureSize = 0.4f;
    float thinMinWidth = 2.0f;
    float layerHeight = 0.2f;
    float settingsAccentHue = 220.0f;
    float settingsAccentSat = 0.35f;
    bool settingsAccentUseSystem = true;

    // Calibration tool — unified face-pick flow; `calibWorkflow` from CalibrateDistance rules (see CalibDistanceType.hpp).
    CalibWorkflow calibWorkflow = CalibWorkflow::None;
    float calibNominal = 0.0f;    // auto-detected from geometry (mm)
    float calibMeasured = 100.0f; // user-entered printed measurement (mm)
    /// Derived from workflow + nominal + measured (see RefreshCalibCompensation).
    float calibContourScale = 1.0f;
    float calibHoleOffsetMm = 0.0f;
    float calibElephantFootMm = 0.0f;
    bool calibCompensationValid = false;

    // Step indicator states — read per-frame by CheckBox lambdas; update in-place, no rebuild needed
    Icons::StepState calibStepImport    = Icons::StepState::Active;
    Icons::StepState calibStepPoint1    = Icons::StepState::Active;
    Icons::StepState calibStepPoint2    = Icons::StepState::Active;
    Icons::StepState calibStepMeasure   = Icons::StepState::Active;
    Icons::StepState analysisStepImport = Icons::StepState::Active;

    bool settingsOpenAccentPicker = false;
    Select *uiAppearanceThemeSelect = nullptr;
    Select *uiAppearanceAccentSelect = nullptr;

    RootPanel *uiFiles = nullptr;
    RootPanel *uiAnalysis = nullptr;
    RootPanel *uiCalibrate = nullptr;
    RootPanel *uiSettings = nullptr;
    RootPanel *uiToolbar = nullptr;
    SectionLine *toolbarAnalysisLine = nullptr;
    SectionLine *toolbarCalibrateLine = nullptr;
    Paragraph *uiResult = nullptr;
    Paragraph *uiImportPara = nullptr;
    Paragraph *uiVerdict = nullptr;
    // Calibrate step-flow pointers — updated when picking is wired
    Paragraph *calibPara_Import = nullptr;          // hidden after file import
    Paragraph *calibPara_Point1 = nullptr;          // shown/activated after import
    Paragraph *calibPara_Point2 = nullptr;          // shown after point1 is plotted
    Section *calibSec_Parameters = nullptr;       // hidden until import (contains Print measurement)
    Paragraph *calibPara_Derived = nullptr;       // second Parameters row (span / compensation); hidden when unused
    SectionLine *calibLine_Point1Primary = nullptr; // for per-line state (textDepth etc.)
    SectionLine *calibLine_Point2Primary = nullptr;

    std::vector<std::string> openFiles;

    // Per-flaw live state — written by UpdateScene, read by imguiContent lambdas each frame
    struct FlawResult
    {
        size_t count = 0;
        std::function<void()> frameCallback; // nullptr = no valid bounds
        // Per-row interactive state (must NOT be static in lambdas — all rows share the same lambda body)
        bool requestEdit = false;
        bool editing = false;
        bool tracking = false;
        bool navTracking = false;
        bool focusPending = false;
        ImVec2 startPos = {};
        ImVec2 navStart = {};
    };
    FlawResult flawOverhang, flawSharp, flawThin, flawSmall;

    bool lastVerdictWasPass = false;
    std::string cachedTip;

    void RebuildAnalysis();
    void snapInput(float &x, float &y);
    void InitUI();
    void DoFileImport();
    void RebuildFileTabs();
    void LoadSettings();
    void SaveSettings();
    bool pendingFileTabsRebuild = false;
    bool pendingToolSwitch = false;

    const Face *hoverPickFace = nullptr;
    const Edge *hoverPickEdge = nullptr;
    std::vector<Vertex> pickHighlightVertices;
    std::vector<uint32_t> pickHighlightIndices;
    std::vector<Vertex> pickHighlightLineVertices;
    std::vector<uint32_t> pickHighlightLineIndices;

    void ClearPickHover();
    void ClearCalibrateFacePicks();
    void SetHoverCalibPick(const Face *face, const Edge *edge);
    void RebuildPickHighlightMesh();
    void RefreshCalibWorkflow();
    void RefreshCalibCompensation();
    void RefreshCalibDerivedRowVisible();

    struct CalibPickHit
    {
        const Face *face = nullptr;
        const Edge *edge = nullptr;
    };
    CalibPickHit PickCalibrateAtPixel(float pixelX, float pixelY) const;

    const Face *calibFacePoint1 = nullptr;
    const Face *calibFacePoint2 = nullptr;
    const Edge *calibEdgePoint1 = nullptr;
    const Edge *calibEdgePoint2 = nullptr;

    /// World-space axis half-length used for clip + axis mesh; `NaN` = not synced yet.
    float lastSyncedAxisWorldHalfExtent = std::numeric_limits<float>::quiet_NaN();
    /// Rebuilds axis line length when zoom/aspect/grid change; returns extent for ortho near/far.
    float SyncViewportAxisForDepthClip();
};