#pragma once

#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <chrono>
#include <cstdint>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "utils/utils.hpp"
#include "utils/TaskRunner.hpp"
#include "utils/MainThreadPipeline.hpp"
#include "rendering/SceneRenderer/SceneRenderer.hpp"
#include "rendering/ViewportRenderer/ViewportRenderer.hpp"
#include "rendering/UIRenderer/UIRenderer.hpp"
#include "rendering/UIRenderer/ToolUserErrorFeedback.hpp"
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
struct Solid;
struct ImportProgress;

class Input;

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
    void MarkStyleDirty();
    void MarkGeometryDirtyAll();
    void MarkGeometryDirtySolid(const Solid *solid);
    void MarkPickDirty();
    void Frame();

    bool HitTestUI(float pixelX, float pixelY) const;
    /// True if (pixelX, pixelY) lies over a Dear ImGui window that accepts mouse input (uses last frame rects).
    bool HitTestImGui(float pixelX, float pixelY) const;

    void SetAspectRatio(uint16_t width, uint16_t height);

    void UpdateCamera();

    static bool ResizeEventWatcher(void *userdata, SDL_Event *event);
    Camera GetCamera() { return camera; }
    void Zoom(const float offsetY, const glm::vec3 &posCursotr);
    void BeginOrbitSnapGesture();
    void Orbit(float offsetX, float offsetY);
    void FinishOrbitSnap();
    void Roll(float delta);
    void Pan(float offsetX, float offsetY, bool scroll = true);
    void FrameScene();
    void ResetCameraView();
    glm::vec3 ScreenToWorld(float pixelX, float pixelY) const;
    void MarkBug();

    /// Fills `out` with scene counts, analysis params, camera pose, clip planes, window sizes, and tool flags for logging.
    void FillSessionReproState(SessionState &out) const;

    /// Face hover for tools that use `GetActivePickFilter()` (Calibrate, Structure). Respects UI hit-test and ImGui capture.
    void UpdatePickHover(float pixelX, float pixelY);
    /// Left-click in viewport: commits hovered face to the active Calibrate point prerequisite when applicable (face-only).
    void TryCommitCalibrateFacePick(float pixelX, float pixelY);
    /// Left-click in viewport: commits hovered face as the Structure tool's selected face (single-slot) when eligible.
    void TryCommitStructureFacePick(float pixelX, float pixelY);
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
        Calibrate,
        Structure
    };
    ActiveTool activeTool = ActiveTool::Analysis;

    // Apply the resolved appearance (dark/light) from themeMode + current system theme.
    // Call at startup, on ThemeMode change, and on SDL_EVENT_SYSTEM_THEME_CHANGED.
    void ApplyTheme();

    /// Cycles the Structure debug preview stage (D key):
    ///   0 = cut outline only  1 = notched/no-fillet  2 = all strut candidates  3 = full
    void ToggleStructureDebugCutOutlineMode();

    /// Toggles the Analysis debug overlay (grave key while Analysis tool active): wireframe of the
    /// raw sliced cross-section loops every detector operates on, one layer at a time (Up/Down to step).
    void ToggleAnalysisDebugSlicedView();
    bool IsAnalysisDebugViewEnabled() const { return analysisDebugViewEnabled; }
    /// Steps the visible debug layer by `delta` (Up = +1, Down = -1), clamped to the cached layer range.
    void StepAnalysisDebugLayer(int delta);

    /// Non-owning; used to reset finger/trackpad state when SDL event queue is flushed after import.
    void SetInput(Input *input) { inputForGestureSync = input; }

private:
    enum class InvalidationNode : uint8_t
    {
        Geometry = 0,
        Style,
        Pick,
        Analysis,
        UI,
        Count
    };

    struct InvalidationStats
    {
        uint64_t scheduled[static_cast<size_t>(InvalidationNode::Count)] = {};
        uint64_t executed[static_cast<size_t>(InvalidationNode::Count)] = {};
        uint64_t skipped[static_cast<size_t>(InvalidationNode::Count)] = {};
        uint64_t guardrailViolations = 0;
        uint64_t fallbackToFullRebuild = 0;
    };

    using InvalidationMask = uint32_t;
    static constexpr InvalidationMask NodeBit(InvalidationNode n)
    {
        return 1u << static_cast<uint32_t>(n);
    }

    int16_t windowWidth;
    int16_t windowHeight;
    SDL_Window *InitWindow(int16_t width, int16_t height, const char *title);
    /// Upload Structure preview fill mesh to the renderer. Recomputes the **work order** (eligible
    /// faces minus exclusions) and resets incremental bake state when it changes; heavy per-face
    /// `BuildCutOutlinePreviewLines` runs in `TickStructurePreviewBuildIfNeeded` (after pick rebuild)
    /// capped per frame.
    void RefreshStructurePreviewForRenderer();
    /// Re-slices every solid and caches the per-solid layer lists (`analysisDebugLayersBySolid`),
    /// then rebuilds the visible-layer segments. Clears everything when the toggle is off or the
    /// Analysis tool is not active. Call when geometry, scene, or the toggle changes.
    void RefreshAnalysisDebugSlicedView();
    /// Rebuilds `analysisDebugSlicedSegments` from the already-cached `analysisDebugLayersBySolid`
    /// for the current `analysisDebugLayerIndex` and pushes to the renderer. No re-slicing.
    void RebuildAnalysisDebugSegmentsForCurrentLayer();
    /// Sets the Analysis panel header trailing text to "[layer i/N]" (or "[no layers]").
    void UpdateAnalysisDebugHeaderTrailing();
    /// Structure scene-edit footer: `accepted` commits the staging carve into the active scene; otherwise the
    /// pre-Structure original is restored. Both paths leave the tool.
    void FinalizeStructureSceneToolSession(bool accepted);
    /// Build the temporary carved staging scene from the current imported scene and swap it into
    /// `ownedScenes[activeSceneIndex]` so the user sees the carve live. The original is held aside in
    /// `structureOriginalScene` for cancel/restore. The OCCT carve runs on a worker thread;
    /// the swap happens on the main thread when the job completes. Main-thread `Scene::Clone` + job submit
    /// run on the **next** `Frame()` tick (see `StructureCarvePipelinePhase::LaunchPending`) so tool entry can paint
    /// once before clone. No-op when no model is loaded or staging is already active.
    void BeginStructureStagingSession();
    /// Restore the pre-staging original scene if a staging session is active; cleared otherwise. Re-runs
    /// `UpdateScene()` so the renderer/pick state rebuild from the original.
    void RestoreStructureOriginalScene();
    /// Drop the held original (commit). Caller must have verified staging is active. Leaves the staging
    /// in place inside `ownedScenes[activeSceneIndex]`.
    void CommitStructureStagingScene();
    /// Convenience for "params changed, re-bake the staging from the untouched original": cancel and
    /// re-enter staging in one step. No-op when no staging session is active. Hook for future inset /
    /// per-face exclusion changes; currently nothing in the UI calls it.
    void RebuildStructureStagingScene();
    /// True while `structureOriginalScene` is held (i.e. the active scene is the carved staging copy).
    bool IsStructureStagingActive() const { return structureOriginalScene != nullptr; }
    /// Import row vs scene-edit footer visibility (footer only when prerequisites are satisfied).
    void SyncStructurePanelDerivedVisibility();
    void SyncStructureOptionalPrereqRowStyle();
    /// Re-evaluate `OpenBoundary` on the active `scene` and update `calibStepImportClosedVolume` + payload.
    void RefreshImportClosedVolumeContractFromScene() noexcept;
    void SyncCalibrateImportPrerequisiteVisibility();
    void RebuildImportOpenBoundaryBlameEdges();
    void RebuildOpenBoundaryBlameFaceGpuMesh();
    void RebuildOpenBoundaryBlameLineGpuMesh();
    bool TryFixOpenBoundaryForActiveScene();
    bool TryFixSelfIntersectionForActiveScene();
    [[nodiscard]] bool ImportAllowsGeometryDependentTools() const noexcept;
    SDL_Window *window = nullptr;
    SDL_GLContext glContext = nullptr;
    Input *inputForGestureSync = nullptr;

    SceneRenderer renderer;
    ViewportRenderer viewportRenderer;
    UIRenderer uiRenderer;
    Scene baseScene;                                 // empty base — active when no file is loaded
    std::vector<std::unique_ptr<Scene>> ownedScenes; // one per imported file
    size_t activeSceneIndex = SIZE_MAX;              // SIZE_MAX = base scene active
    Scene *scene = nullptr;                          // pointer to active scene

    Camera camera;
    bool orbitSnapGestureActive = false;
    Camera::OrbitSnapAxis orbitSnapSuppressedAxis = Camera::OrbitSnapAxis::None;

    bool analysisEnabled = true;
    bool cameraDirty = true;
    // Phase-6 hook: keep CPU-first path by default; shader/material style propagation can gate on this later.
    bool preferShaderStylePropagation = false;
    InvalidationMask scheduledNodes = 0;
    InvalidationStats invalidationStats;
    bool styleDirty = true;
    bool geometryDirtyAll = true;
    std::unordered_set<const Solid *> geometryDirtySolids;
    bool pickDirty = true;

    float overhangAngle = 45.0f;
    float sharpCornerThreshold = 54.0f;
    float notEnoughSpaceThreshold = 54.0f;
    float instabilityMinWidth = 2.0f;
    float layerDifferenceMaxAreaDelta = 50.0f;
    float layerHeight = 0.2f;
    /// Structure tool inset distance in millimetres. Drives the CGAL straight-skeleton offset, the
    /// strip-band width, and the fillet radius (1:1). Slider lives in the Structure tool panel;
    /// dragging it invalidates the per-face bake cache via
    /// `StructureTriangulation::InvalidateBakeCacheForParams` (wired in B2f).
    float structureInsetMm = 2.0f;
    float settingsAccentHue = 220.0f;
    float settingsAccentSat = 0.35f;
    bool settingsAccentUseSystem = true;

    // Calibration tool — unified face-pick flow; `calibWorkflow` from CalibrateDistance rules (see CalibDistanceType.hpp).
    CalibWorkflow calibWorkflow = CalibWorkflow::None;
    float calibNominal = 0.0f;    // auto-detected from geometry (mm)
    float calibMeasured = 100.0f; // user-entered printed measurement (always stored in mm)
    /// Derived from workflow + nominal + measured (see RefreshCalibCompensation).
    float calibContourScale = 1.0f;
    float calibHoleOffsetMm = 0.0f;
    float calibElephantFootMm = 0.0f;
    bool calibCompensationValid = false;
    /// Calibrate panel: stable code + copyable message when span/compensation cannot be computed.
    std::optional<ToolUserErrorPayload> calibToolError;
    /// Structure panel: CGAL / carve failure surfaced after worker result is applied on the main thread.
    std::optional<ToolUserErrorPayload> structureToolError;
    /// Post-import open-boundary banner (Calibrate panel); cleared when contract satisfied.
    std::optional<ToolUserErrorPayload> importOpenBoundaryToolPayload;
    bool importOpenBoundaryBannerDismissed = false;
    std::string openBoundaryDiagLastSignature;
    /// Segments for `OpenBoundary` “blame” overlay (dedicated line pass).
    /// `importOpenBoundaryContextEdges` adds inferred perimeter context around strict boundary edges.
    std::vector<const Edge *> importOpenBoundaryBlameEdges;
    std::vector<const Edge *> importOpenBoundaryContextEdges;
    std::vector<Vertex> openBoundaryBlameFaceVertices;
    std::vector<uint32_t> openBoundaryBlameFaceIndices;
    std::vector<Vertex> openBoundaryBlameLineVertices;
    std::vector<uint32_t> openBoundaryBlameLineIndices;

    // Step indicator states — read per-frame by CheckBox lambdas; update in-place, no rebuild needed
    Icons::StepState calibStepImport    = Icons::StepState::Active;
    /// After a file is attached: **Done** when no `Solid` in the active scene reports `OpenBoundary`
    /// (fresh `GeometryValidity` cache); **Active** until repair clears the tag. Gates tool picks
    /// together with `calibStepImport` — see `import_open_boundary_ux_2026-05-14.md`.
    Icons::StepState calibStepImportClosedVolume = Icons::StepState::Done;
    Icons::StepState calibStepPoint1    = Icons::StepState::Active;
    Icons::StepState calibStepPoint2    = Icons::StepState::Active;
    Icons::StepState calibStepMeasure   = Icons::StepState::Active;
    Icons::StepState analysisStepImport = Icons::StepState::Active;


    bool settingsOpenAccentPicker = false;
    Select *uiAppearanceThemeSelect = nullptr;
    Select *uiAppearanceAccentSelect = nullptr;
    Select *uiDefaultLengthUnitSelect = nullptr;

    RootPanel *uiFiles = nullptr;
    RootPanel *uiAnalysis = nullptr;
    RootPanel *uiCalibrate = nullptr;
    RootPanel *uiStructure = nullptr;
    RootPanel *uiSettings = nullptr;
    RootPanel *uiToolbar = nullptr;
    SectionLine *toolbarAnalysisLine = nullptr;
    SectionLine *toolbarCalibrateLine = nullptr;
    SectionLine *toolbarStructureLine = nullptr;
    Paragraph *uiResult = nullptr;
    Paragraph *uiImportPara = nullptr;
    Paragraph *uiVerdict = nullptr;
    Paragraph *uiAnalysisProcessing = nullptr;
    Paragraph *uiCalibrateProcessing = nullptr;
    // Calibrate step-flow pointers — updated when picking is wired
    Paragraph *calibPara_Import = nullptr;          // hidden after file import
    Paragraph *calibPara_ImportClosed = nullptr;  // "Closed volume (tools)" prerequisite row
    Paragraph *calibPara_OpenBoundaryBanner = nullptr; // Fix / Exit strip after import when OB
    Paragraph *calibPara_Point1 = nullptr;          // shown/activated after import
    Paragraph *calibPara_Point2 = nullptr;          // shown after point1 is plotted
    Paragraph *calibPara_Measure = nullptr;         // direct Calibrate row; hidden until import
    Section *calibSec_Parameters = nullptr;         // measurement row(s); legacy flat layout if absent
    Section *calibSec_Result = nullptr;             // Calculator section: compensation result row
    Section *calibSec_Prerequisites = nullptr;
    Paragraph *calibPara_Derived = nullptr;         // result row (span / compensation); hidden when unused
    SectionLine *calibLine_Point1Primary = nullptr; // for per-line state (textDepth etc.)
    SectionLine *calibLine_Point2Primary = nullptr;
    /// Structure prerequisites: import row (hidden after import; uses `calibStepImport` like Calibrate).
    Paragraph *structPara_Import = nullptr;
    Paragraph *structPara_ImportClosed = nullptr;
    /// Cancel/Accept row; hidden until import prerequisite is complete.
    Paragraph *structPara_SceneEditFooter = nullptr;
    /// Single-line hover hint for ineligible faces. Currently dormant — `SyncStructurePanel-
    /// DerivedVisibility` keeps the paragraph permanently hidden so the UI no longer leaks
    /// clickability state. The plumbing (this slot + `structureHoverIneligibleReason`) is retained
    /// so a future phase can re-enable the hint by toggling visibility back on.
    Paragraph *structPara_HoverHint = nullptr;
    /// Copyable carve failure (`ToolUserErrorPayload`); shown when `structureToolError` is set.
    Paragraph *structPara_ToolError = nullptr;
    /// Extra-prerequisite row: face exclusions (`ExtraPrerequisites` section).
    Paragraph *structPara_OptionalFaceExclude = nullptr;
    /// Drives the optional row checkbox and whether face clicks may toggle exclusions (`Active` = yes).
    Icons::StepState structureOptFaceExcludeStep = Icons::StepState::Active;

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
    FlawResult flawOverhang, flawSharpCorner, flawNotEnoughSpace, flawInstability, flawLayerDifference;
    const Scene *analysisUiScene = nullptr;

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
    /// Set when the file dialog returns; consumed at the start of `Frame()` (splash + blocking import).
    std::optional<std::string> deferredImportPath;
    struct AsyncImportResult
    {
        bool ok = false;
        bool cancelled = false;
        std::string path;
        std::string lower;
        std::unique_ptr<Scene> importedScene;
        bool hasStlStats = false;
        double stlParseMs = 0.0;
        double stlMergeMs = 0.0;
        double stlTotalMs = 0.0;
        uint32_t stlTriangles = 0;
        std::size_t stlUniquePoints = 0;
        std::size_t stlFaces = 0;
        bool stlIsBinary = false;
        bool hasStlMergeDiagnostics = false;
        MergeCoplanarDiagnostics stlMergeDiagnostics;
        double importerMs = 0.0;
    };
    struct AsyncAnalysisResult
    {
        bool ok = false;
        bool cancelled = false;
        uint64_t requestId = 0;
        const Scene *scene = nullptr;
        AnalysisResults results;
    };

    /// Async job stack (see `documentation/Architecture_AsyncWorkRoadmap.md` and
    /// `Architecture_UIThreadAndWorkers.md`): workers run heavy work; `Frame()` polls `TryTake` and
    /// applies on the main thread. Import uses `mainThreadPipeline` for multi-step attach/update.
    /// Structure CGAL carve uses a separate `TaskRunner` in `display.cpp` so it cannot starve this queue.
    /// `Shutdown()` detaches workers and **releases** the runner (intentional process-exit leak) so quit
    /// never blocks on `join()` when CGAL is stuck.
    std::unique_ptr<TaskRunner> taskRunner;
    MainThreadPipeline mainThreadPipeline;
    std::optional<TaskRunner::TaskHandle<AsyncImportResult>> pendingImportTask;
    std::optional<TaskRunner::TaskHandle<AsyncAnalysisResult>> pendingAnalysisTask;
    /// Main-thread sequencing for Structure carve (deferred clone/submit vs worker future). Keeps UI / preview
    /// gates aligned with `FlushPendingStructureStagingCarveLaunchIfAny` + `PollStructureStagingTaskIfReady`.
    enum class StructureCarvePipelinePhase : uint8_t
    {
        Idle = 0,
        LaunchPending,
        Carving,
    };

    /// Worker output for `BeginStructureStagingSession`: carved staging scene and optional carve error text.
    struct AsyncStructureStagingResult
    {
        bool cancelled = false;
        uint64_t jobId = 0;
        size_t targetSceneIndex = SIZE_MAX;
        std::unique_ptr<Scene> staging;
        std::string firstErr;
        size_t carvedSolids = 0;
        size_t carveAttempts = 0;
    };
    std::optional<TaskRunner::TaskHandle<AsyncStructureStagingResult>> pendingStructureStagingTask;
    /// Incremented when a new carve job is issued or when pending work is invalidated; results must match to apply.
    uint64_t structureStagingIssuedJobId = 0;
    StructureCarvePipelinePhase structureCarvePipelinePhase = StructureCarvePipelinePhase::Idle;
    /// Denominator (solids to carve) for the current job; numerator updated from the worker thread.
    uint64_t structureStagingStepsTotal = 0;
    std::atomic<uint64_t> structureStagingStepsDone{0};
    /// Fine-grained (within-solid) progress snapshot reported by `StructureCarve::TryApplyStructureCarve`'s
    /// `progress` callback, gated by jobId so stale callbacks from a cancelled/replaced job are dropped.
    std::mutex structureProgressMutex;
    uint64_t structureProgressJobId = 0;
    std::string latestStructureProgressPhase;
    float latestStructureProgress01 = -1.0f;
    bool latestStructureProgressDirty = false;
    std::string structureProgressPhase;
    float structureProgress01 = -1.0f;
    const Scene *pendingAnalysisScene = nullptr;
    /// Latest async analysis result awaiting render application (tints / flaw overlay).
    std::optional<AsyncAnalysisResult> pendingAnalysisTint;
    /// Incremental full rebuild spans many frames; keep analysis snapshot + id stable until it finishes.
    std::optional<AnalysisResults> activeAnalysisTintForRebuild;
    uint64_t activeAnalysisTintIdentityForRebuild = 0;
    /// Snapshot of the last analysis applied to the mesh; used by `RecolorOnly` so style-only refreshes do not strip tints.
    std::optional<AnalysisResults> lastCommittedAnalysisForRecolor;
    uint64_t analysisRequestId = 0;
    bool skipAnalysisForNextGeometryRebuild = false;
    bool pendingAnalysisAfterGeometryRebuild = false;
    int analysisProcessingIdleStreak = 0;

    uint64_t analysisPipelineDenomTotal = 0;
    std::atomic<uint64_t> analysisWorkerStepsDone{0};
    std::atomic<uint64_t> analysisTintStepsDone{0};
    std::atomic<uint32_t> analysisWorkerPhaseIdAtomic{0};
    uint64_t analysisTintStepMarkedForRequestId = 0;
    uint64_t analysisGpuRebuildStepsCache = 0;

    void RefreshAnalysisPipelineDenominatorFromScene();
    void ResetAnalysisPipelineTotals();
    void PrimeAnalysisWorkerQueueStepDone();
    void OnAnalysisWorkerSceneStep(uint32_t phaseId, uint64_t intraSceneStepIndex);
    void TryMarkAnalysisTintStepOnce(uint64_t requestId);

    bool importBusy = false;
    float importProgress01 = -1.0f; // in [0,1] for real percent; -1 = indeterminate
    std::string importProgressPhase;
    std::mutex importProgressMutex;
    std::string latestImportProgressPhase;
    float latestImportProgress01 = -1.0f;
    bool latestImportProgressDirty = false;
    std::atomic<uint64_t> importProgressGeneration{0};
    /// Set at the start of `Frame()`: shared end time for bounded `TryTake` on worker futures
    /// (import / analysis / structure) so the UI thread waits at most ~one frame per frame total.
    std::optional<std::chrono::steady_clock::time_point> workerFuturePollDeadline;
    /// File-bar tab stem while async import is active (empty when idle).
    std::string pendingImportTabStem;
    bool pendingImportTabActive = false;

    void ProcessDeferredImportIfAny();
    /// Non-blocking: if `pendingAnalysisTask` finished, apply tint when ids/scene match.
    void PollPendingAnalysisTaskIfReady();
    /// Shared worker body for both analysis `Submit` sites in `Frame()`.
    AsyncAnalysisResult ProduceAsyncAnalysisFromScene(const Scene *sceneForAnalysis, uint64_t requestId,
                                                      const TaskRunner::CancellationToken &token);
    /// After import, flush SDL's finger/mouse tail and sync `Input` (`activeTouches`) — queue vs model must match.
    void FlushImportInputEventTail();
    void PublishImportProgress(uint64_t generation, const ImportProgress &progress);
    void ApplyImportProgressSnapshot();
    void ClearPendingImportProgressSnapshot();
    void PublishStructureCarveProgress(uint64_t jobId, const ImportProgress &progress);
    void ApplyStructureProgressSnapshot();
    void ClearPendingStructureProgressSnapshot();
    [[nodiscard]] std::chrono::milliseconds WorkerFuturePollRemainingMs() const;
    void SetImportProgress(std::string phase, float progress01);
    void SyncToolbarToolVisualState();
    void RefreshToolProcessingCards(bool hasModel, bool geometryOrStyleWork, bool ranMainThreadApplyTask);

    const Face *hoverPickFace = nullptr;
    const Edge *hoverPickEdge = nullptr;
    /// Hover hit failed the current tool's eligibility gate; highlight uses darker **gray** instead of accent.
    bool hoverPickRejected = false;
    std::vector<Vertex> pickHighlightVertices;
    std::vector<uint32_t> pickHighlightIndices;
    std::vector<Vertex> pickHighlightLineVertices;
    std::vector<uint32_t> pickHighlightLineIndices;
    std::vector<Vertex> pickHighlightRejectVertices;
    std::vector<uint32_t> pickHighlightRejectIndices;
    std::vector<Vertex> pickHighlightCalibInvalidVertices;
    std::vector<uint32_t> pickHighlightCalibInvalidIndices;

    bool calibHoverSpanPreviewActive = false;
    std::string calibHoverSpanLabel;
    glm::dvec3 calibHoverSpanP0{};
    glm::dvec3 calibHoverSpanP1{};

    void ClearPickHover();
    void ClearCalibrateFacePicks();
    void ClearStructureFacePick();
    void SetHoverPick(const Face *face, const Edge *edge, bool rejected = false);
    void RebuildPickHighlightMesh();
    void ScheduleNode(InvalidationNode node);
    void ProcessPendingToolSwitchIfAny();
    void RunPickNode();
    void RunUiNode();
    void ClearScheduledNodes();
    void InvalidationSkip(InvalidationNode node);
    void InvalidationExec(InvalidationNode node);
    void InvalidationGuardrailViolation();
    void RefreshCalibWorkflow();
    void RefreshCalibCompensation();
    void RefreshCalibDerivedRowVisible();

    void RefreshCalibSpanOverlayForViewportRender();

    struct CalibPickHit
    {
        const Face *face = nullptr;
        const Edge *edge = nullptr;
    };
    CalibPickHit PickCalibrateAtPixel(float pixelX, float pixelY) const;

    /// Structure tool: single-face pick for face-triangulation (Phase B). `ineligibleReason` is set
    /// when a ray hits a face that fails the eligibility gate (non-planar, multi-loop, downward/vertical
    /// normal, too small) so callers can surface the reason to the user.
    struct StructurePickHit
    {
        const Face *face = nullptr;
        bool eligible = false;
        std::string ineligibleReason;
    };
    StructurePickHit PickStructureAtPixel(float pixelX, float pixelY) const;
    /// Returns true when `face` is a candidate for face triangulation under the Structure tool. The
    /// reason string is populated for every rejection (used by the panel hint and click logging).
    bool IsStructureFaceEligible(const Face *face, std::string *outReason = nullptr) const;
    /// Rebuilds `structureCarvedToOriginal` by geometric (plane + centroid) match between the
    /// current carved `scene` and `structureOriginalScene`. Call once whenever a new carve result is
    /// applied — the matching itself is unavoidably approximate (the cut/unify pipeline keeps no
    /// provenance), but doing it once per carve instead of per pick/per frame is what makes that cost
    /// acceptable.
    void RebuildStructureCarvedToOriginalMap();

    const Face *calibFacePoint1 = nullptr;
    const Face *calibFacePoint2 = nullptr;
    const Edge *calibEdgePoint1 = nullptr;
    const Edge *calibEdgePoint2 = nullptr;

    /// Held while a Structure staging session is active: the pre-carve imported scene moved out of
    /// `ownedScenes[activeSceneIndex]`. On Cancel we move it back and the staging is discarded; on
    /// Accept we drop it and the staging stays in place. `nullptr` outside the session.
    std::unique_ptr<Scene> structureOriginalScene;
    /// Maps each eligible carved-result `Face*` (in the current staged `scene`) to the original
    /// `Face*` it best corresponds to, or omits faces with no original counterpart (new carve
    /// geometry — strut walls, cut boundaries). Built once per carve by `RebuildStructureCarvedToOriginalMap`;
    /// picking/hover/tint look this up directly instead of ray-casting or re-matching per frame.
    std::unordered_map<const Face *, const Face *> structureCarvedToOriginal;
    /// Tab slot the staging is occupying; only meaningful while `structureOriginalScene != nullptr`.
    size_t structureStagingSceneIndex = SIZE_MAX;
    /// Structure tool: opt-out exclusion set keyed on **original** scene faces (held in
    /// `structureOriginalScene` while staging).
    std::unordered_set<const Face *> structureExcludedFaces;
    /// Cache of eligible-face pointers for the current scene. Rebuilt lazily in
    /// `RebuildPickHighlightMesh` when the Structure tool is active so the render loop can apply the
    /// "included by default" tint without re-running the eligibility gate per triangle.
    std::unordered_set<const Face *> structureEligibleFacesCache;
    /// Last hover ineligibility reason. Dormant at this phase — nothing writes to it because the
    /// clickability hint paragraph is hidden (see `structPara_HoverHint`). Kept for cheap re-
    /// enablement later: a future `UpdatePickHover`/`TryCommitStructureFacePick` change can start
    /// writing to it and `SyncStructurePanelDerivedVisibility` can route the string into the row.
    std::string structureHoverIneligibleReason;
    /// Set when Structure **Accept** finalizes: the carved mesh is already on screen; the next tool-switch
    /// pass should not call `MarkGeometryDirtyAll` (that would replay a full incremental GPU rebuild).
    bool structureFinalizeCommitSkipGpuFullRebuild = false;
    /// After Structure Accept, GPU buffers already match the carved scene; the first analysis tint apply
    /// should recolor only, not force another full incremental rebuild.
    bool structureAcceptGpuGeometryFresh = false;

    /// Spreads `StructureTriangulation::BuildCutOutlinePreviewLines` across frames so OCCT-heavy
    /// models do not block the UI thread on a single eligibility refresh. See `TickStructurePreviewBuildIfNeeded`.
    void ResetStructurePreviewIncrementalState();
    void TickStructurePreviewBuildIfNeeded();
    void CollectStructurePreviewWorkOrder(std::vector<const Face *> &out) const;
    bool StructurePreviewBakeSnapshotMatches(const std::vector<const Face *> &sortedFaces, double insetMm) const;
    void AdvanceStructurePreviewBuild(double insetMm);
    /// Live segments for the given debug stage (0–3).
    const std::vector<std::pair<glm::vec3, glm::vec3>> &StageSegments(int stage) const;
    /// Snapshot segments for the given debug stage (survive staging/carve clears).
    const std::vector<std::pair<glm::vec3, glm::vec3>> &StageSnapshot(int stage) const;
    static constexpr std::size_t kStructurePreviewMaxFacesPerFrame = 8;
    std::vector<const Face *> structurePreviewBakeQueue;
    std::size_t structurePreviewBakeCursor = 0;
    std::vector<std::pair<glm::vec3, glm::vec3>> structurePreviewBakedSegments;          // stage 3: full
    std::vector<std::pair<glm::vec3, glm::vec3>> structurePreviewCutOutlineSegments;      // stage 0: base outline
    std::vector<std::pair<glm::vec3, glm::vec3>> structurePreviewNotchedSegments;         // stage 1: notched, no fillet
    std::vector<std::pair<glm::vec3, glm::vec3>> structurePreviewAllCandidatesSegments;   // stage 2: all valid strut pairs
    std::vector<std::pair<glm::vec3, glm::vec3>> structurePreviewStrutQuadSegments;       // stage 4: unclipped quad outlines
    /// Snapshots survive staging/carve clears so ` keeps working after a carve starts.
    std::vector<std::pair<glm::vec3, glm::vec3>> structureDebugCutOutlineSnapshot;
    std::vector<std::pair<glm::vec3, glm::vec3>> structureDebugNotchedSnapshot;
    std::vector<std::pair<glm::vec3, glm::vec3>> structureDebugAllCandidatesSnapshot;
    std::vector<std::pair<glm::vec3, glm::vec3>> structureDebugStrutQuadSnapshot;
    std::vector<std::pair<glm::vec3, glm::vec3>> structureDebugFullSnapshot;
    /// 0=cut outline  1=no fillet  2=all candidates  3=full  4=strut quads
    int structureDebugPreviewStage = 0;
    double structurePreviewBakeInsetMm = std::numeric_limits<double>::quiet_NaN();

    /// Analysis debug overlay: steps through one sliced cross-section layer at a time.
    bool analysisDebugViewEnabled = false;
    /// Cached per-solid layer lists from the last `RefreshAnalysisDebugSlicedView`, parallel to
    /// `scene->solids` order; avoids re-slicing on every Up/Down step.
    std::vector<std::vector<SlicedLayer>> analysisDebugLayersBySolid;
    int analysisDebugLayerIndex = 0;
    std::vector<std::pair<glm::vec3, glm::vec3>> analysisDebugSlicedSegments;

    /// World-space axis half-length used for clip + axis mesh; `NaN` = not synced yet.
    float lastSyncedAxisWorldHalfExtent = std::numeric_limits<float>::quiet_NaN();
    /// Rebuilds axis line length when zoom/aspect/grid change; returns extent for ortho near/far.
    float SyncViewportAxisForDepthClip();
    /// Grid cell size (`Color::GRID_CELL_SIZE`) from default length unit; half-extent from `settings.gridCellsAlongAxis`.
    void SyncGridLayoutFromSettings();

    void PollStructureStagingTaskIfReady();
    /// Swaps `r.staging` into the active scene (or reports the carve error) once a carve result is ready.
    /// Shared by the async worker poll path and the synchronous main-thread carve path.
    void ApplyStructureStagingResult(AsyncStructureStagingResult &&r);
    /// Cancels an in-flight carve job (if any), bumps `structureStagingIssuedJobId`, clears the header busy hint.
    void CancelPendingStructureCarveJob();
    void FlushPendingStructureStagingCarveLaunchIfAny();
    void LaunchStructureStagingCarveJob();
};