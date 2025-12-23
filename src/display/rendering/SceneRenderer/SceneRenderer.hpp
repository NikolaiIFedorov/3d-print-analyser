#pragma once

#include "glad/glad.h"
#include "GLFW/glfw3.h"

#include "rendering/OpenGL/OpenGLRenderer.hpp"
#include "rendering/Camera/camera.hpp"
#include "rendering/SceneRenderer/color.hpp"

#include "RenderBuffer/RenderBuffer.hpp"
#include "Wireframe/Wireframe.hpp"
#include "Patch/patch.hpp"

#include "scene/scene.hpp"
#include "scene/Id.hpp"

#include "mapbox/earcut.hpp"

enum class RenderMode
{
    WIREFRAME,
    SOLID,
    WIREFRAME_ON_SOLID
};

class SceneRenderer
{
public:
    Wireframe wireframe;
    Patch patch;

    SceneRenderer(GLFWwindow *window);

    void SetRenderMode(RenderMode mode);

    void UpdateFromRenderBuffer(const Scene &scene);

    void SetCamera(Camera &camera);

    void Render();

    void Shutdown();

    void AddForm(uint32_t id);

    RenderBuffer renderBuffer;

private:
    OpenGLRenderer renderer;
    GLFWwindow *window = nullptr;
    RenderMode currentMode = RenderMode::WIREFRAME_ON_SOLID;
};