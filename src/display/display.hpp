#pragma once

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <string>
#include "utils/utils.hpp"
#include "rendering/SceneRenderer/SceneRenderer.hpp"

class Display
{
public:
    Display(int16_t width, int16_t height, const char *title);
    void Shutdown();

    GLFWwindow *GetWindow() { return window; }
    SceneRenderer *GetRenderer() { return &renderer; }

    void Render(const Scene &scene);
    void AddForm(uint32_t id);
    void AddForm(const std::vector<uint32_t> &ids);
    void UpdateBuffer(const Scene &scene);

    void SetAspectRatio(uint16_t width, uint16_t height);

    void UpdateCamera();
    Camera GetCamera() { return camera; }
    void Zoom(const float offsetY, const glm::vec3 &posCursotr);
    void Orbit(const float offsetX, const float offsetY);
    void Pan(const float offsetX, const float offsetY);

private:
    int16_t windowWidth;
    int16_t windowHeight;
    GLFWwindow *GetWindow(int16_t width, int16_t height, const char *title);
    GLFWwindow *window = nullptr;

    uint8_t fps;

    SceneRenderer renderer;

    Camera camera;
};