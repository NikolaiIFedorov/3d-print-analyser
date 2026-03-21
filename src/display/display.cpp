#include "display.hpp"

#include <thread>

Display::Display(int16_t width, int16_t height, const char *title) : window(InitWindow(width, height, title)), renderer(GetWindow()), camera(width, height)
{
    SDL_DisplayID displayID = SDL_GetPrimaryDisplay();
    if (displayID == 0)
    {
        LOG_VOID("Display is null");
        return;
    }

    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(displayID);
    if (mode == nullptr)
    {
        LOG_VOID("Mode is null");
        return;
    }

    fps = static_cast<uint8_t>(mode->refresh_rate);
    LOG_DESC("Monitor FPS: " + Log::NumToStr(fps));

    LOG_VOID("Initialized display");
}

SDL_Window *Display::InitWindow(int16_t width, int16_t height, const char *title)
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        LOG_FALSE("Failed to initialize SDL: " + std::string(SDL_GetError()));
        return nullptr;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

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
    if (glContext)
        SDL_GL_DestroyContext(glContext);
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();
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

void Display::Zoom(const float offsetY, const glm::vec3 &posCursotr)
{
    camera.Zoom(offsetY, posCursotr);
}

void Display::snapInput(float &x, float &y)
{
    if (std::abs(x) <= std::abs(y) * 2)
        x = 0;
    else if (std::abs(y) <= std::abs(x) * 2)
        y = 0;
}

void Display::Orbit(float offsetY, float offsetX)
{
    snapInput(offsetX, offsetY);
    camera.Orbit(offsetY, offsetX);
}

void Display::Pan(float offsetX, float offsetY, bool scroll)
{
    snapInput(offsetX, offsetY);
    camera.Pan(offsetX, offsetY, scroll);
}