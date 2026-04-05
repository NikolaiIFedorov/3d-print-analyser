#pragma once

#include "glad/glad.h"
#include "SDL3/SDL.h"

#include "rendering/OpenGL/OpenGLRenderer.hpp"
#include "rendering/Camera/camera.hpp"
#include "rendering/color.hpp"

#include "Wireframe/Wireframe.hpp"
#include "Patch/patch.hpp"

#include "scene/scene.hpp"

#include "mapbox/earcut.hpp"

class SceneRenderer
{
public:
    Wireframe wireframe;
    Patch patch;

    SceneRenderer() {};

    SceneRenderer(SDL_Window *window) : renderer(window) {};

    void UpdateScene(Scene *scene);

    void SetCamera(Camera &camera);

    void Render();

    void Shutdown();

private:
    OpenGLRenderer renderer;
    SDL_Window *window = nullptr;
};