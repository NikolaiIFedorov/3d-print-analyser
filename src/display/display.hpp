#pragma once

#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <string>
#include "utils/utils.hpp"
#include "rendering/SceneRenderer/SceneRenderer.hpp"
#include "rendering/UIRenderer/UIRenderer.hpp"

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
    bool HandleClick(float pixelX, float pixelY);

    void SetAspectRatio(uint16_t width, uint16_t height);

    void UpdateCamera();
    Camera GetCamera() { return camera; }
    void Zoom(const float offsetY, const glm::vec3 &posCursotr);
    void Orbit(float offsetX, float offsetY);
    void Roll(float delta);
    void Pan(float offsetX, float offsetY, bool scroll = true);

private:
    int16_t windowWidth;
    int16_t windowHeight;
    SDL_Window *InitWindow(int16_t width, int16_t height, const char *title);
    SDL_Window *window = nullptr;
    SDL_GLContext glContext = nullptr;

    SceneRenderer renderer;
    UIRenderer uiRenderer;
    Scene *scene = nullptr;

    Camera camera;

    bool cameraDirty = true;
    bool sceneDirty = true;

    void snapInput(float &x, float &y);
    void InitUI();
};