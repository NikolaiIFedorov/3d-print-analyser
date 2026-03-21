#pragma once

#include <glad/glad.h>
#include <SDL3/SDL.h>
#include <string>
#include "utils/utils.hpp"
#include "rendering/SceneRenderer/SceneRenderer.hpp"

class Display
{
public:
    Display(int16_t width, int16_t height, const char *title);
    void Shutdown();

    SDL_Window *GetWindow() { return window; }
    SceneRenderer *GetRenderer() { return &renderer; }

    void Render(const Scene &scene);
    void AddForm(FormPtr form);
    void AddForm(const std::vector<FormPtr> &forms);
    void UpdateBuffer(const Scene &scene);

    void SetAspectRatio(uint16_t width, uint16_t height);

    void UpdateCamera();
    Camera GetCamera() { return camera; }
    void Zoom(const float offsetY, const glm::vec3 &posCursotr);
    void Orbit(float offsetX, float offsetY);
    void Pan(float offsetX, float offsetY, bool scroll = true);

private:
    int16_t windowWidth;
    int16_t windowHeight;
    SDL_Window *InitWindow(int16_t width, int16_t height, const char *title);
    SDL_Window *window = nullptr;
    SDL_GLContext glContext = nullptr;

    uint8_t fps;

    SceneRenderer renderer;

    Camera camera;

    void snapInput(float &x, float &y);
};