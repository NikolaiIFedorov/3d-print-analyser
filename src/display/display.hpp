#pragma once

#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <string>
#include "utils/utils.hpp"
#include "rendering/SceneRenderer/SceneRenderer.hpp"
#include "rendering/AnalysisRenderer/AnalysisRenderer.hpp"
#include "rendering/ViewportRenderer/ViewportRenderer.hpp"
#include "rendering/UIRenderer/UIRenderer.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"

class Display
{
public:
    Display(int16_t width, int16_t height, const char *title, Scene *scene);
    void Shutdown();

    SDL_Window *GetWindow() { return window; }
    SceneRenderer *GetRenderer() { return &renderer; }
    UIRenderer *GetUIRenderer() { return &uiRenderer; }

    void Render();
    void UpdateScene();
    void Frame();

    bool HitTestUI(float pixelX, float pixelY) const;

    void SetAspectRatio(uint16_t width, uint16_t height);

    void UpdateCamera();

    static bool ResizeEventWatcher(void *userdata, SDL_Event *event);
    Camera GetCamera() { return camera; }
    void Zoom(const float offsetY, const glm::vec3 &posCursotr);
    void Orbit(float offsetX, float offsetY);
    void Roll(float delta);
    void Pan(float offsetX, float offsetY, bool scroll = true);
    void FrameScene();
    glm::vec3 ScreenToWorld(float pixelX, float pixelY) const;

    bool renderDirty = true;

private:
    int16_t windowWidth;
    int16_t windowHeight;
    SDL_Window *InitWindow(int16_t width, int16_t height, const char *title);
    SDL_Window *window = nullptr;
    SDL_GLContext glContext = nullptr;

    SceneRenderer renderer;
    AnalysisRenderer analysisRenderer;
    ViewportRenderer viewportRenderer;
    UIRenderer uiRenderer;
    Scene *scene = nullptr;

    Camera camera;

    bool analysisEnabled = true;
    bool cameraDirty = true;
    bool sceneDirty = true;

    float overhangAngle = 45.0f;
    float sharpCornerAngle = 100.0f;
    float minFeatureSize = 1.5f;
    float layerHeight = 0.2f;

    Paragraph *uiResult = nullptr;
    Paragraph *uiImportPara = nullptr;
    Paragraph *uiVerdict = nullptr;

    // Per-flaw live state — written by UpdateScene, read by imguiContent lambdas each frame
    struct FlawResult
    {
        size_t count = 0;
        std::function<void()> frameCallback; // nullptr = no valid bounds
        // Per-row interactive state (must NOT be static in lambdas — all rows share the same lambda body)
        bool requestEdit  = false;
        bool editing      = false;
        bool tracking     = false;
        bool navTracking  = false;
        bool focusPending = false;
        ImVec2 startPos   = {};
        ImVec2 navStart   = {};
    };
    FlawResult flawOverhang, flawSharp, flawThin, flawSmall;

    bool lastVerdictWasPass = false;
    std::string cachedTip;

    void RebuildAnalysis();
    void snapInput(float &x, float &y);
    void InitUI();
};