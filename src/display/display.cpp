#include "display.hpp"

#include <thread>

Display::Display(int16_t width, int16_t height, const char *title) : window(GetWindow(width, height, title)), renderer(GetWindow()), camera(width, height)
{
    GLFWmonitor *monitor = glfwGetPrimaryMonitor();

    if (monitor == nullptr)
    {
        LOG_VOID("Monitor is null");
        return;
    }

    const GLFWvidmode *mode = glfwGetVideoMode(monitor);

    if (mode == nullptr)
    {
        LOG_VOID("Mode is null");
        return;
    }

    fps = mode->refreshRate;
    LOG_DESC("Monitor FPS: " + Log::NumToStr(fps));
    LOG_VOID("Initialized display");
}

GLFWwindow *Display::GetWindow(int16_t width, int16_t height, const char *title)
{
    if (!glfwInit())
    {
        LOG_FALSE("Failed to initialize GLFW");
        return nullptr;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    glfwWindowHint(GLFW_DEPTH_BITS, 24);

    windowWidth = width;
    windowHeight = height;

    GLFWwindow *w = glfwCreateWindow(windowWidth, windowHeight, title, nullptr, nullptr);

    glfwMakeContextCurrent(w);
    glfwSwapInterval(1);

    return w;
}

void Display::AddForm(FormPtr form)
{
    renderer.AddForm(form);
}

void Display::AddForm(const std::vector<FormPtr> &forms)
{
    for (FormPtr form : forms)
    {
        renderer.AddForm(form);
    }
}

void Display::Shutdown()
{
    renderer.Shutdown();
    glfwTerminate();
}

void Display::UpdateCamera()
{
    renderer.SetCamera(camera);
}

void Display::Render(const Scene &scene)
{
    renderer.Render();
}

void Display::UpdateBuffer(const Scene &scene)
{
    renderer.UpdateFromRenderBuffer(scene);
}

void Display::SetAspectRatio(const uint16_t width, const uint16_t height)
{
    glViewport(0, 0, width, height);
    camera.SetAspectRatio(static_cast<float>(width) / static_cast<float>(height));
}

void Display::Zoom(float offsetY, const glm::vec3 &posCursotr)
{
    camera.Zoom(offsetY, posCursotr);
}

void Display::Orbit(const float offsetY, const float offsetX)
{
    camera.Orbit(offsetY, offsetX);
}

void Display::Pan(const float offsetX, const float offsetY)
{
    camera.Pan(offsetX, offsetY);
}