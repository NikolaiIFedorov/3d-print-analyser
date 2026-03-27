#include "display.hpp"

Display::Display(int16_t width, int16_t height, const char *title, Scene *scene) : window(InitWindow(width, height, title)), renderer(GetWindow()), camera(width, height), scene(scene)
{
    LOG_VOID("Initialized display");
}

SDL_Window *Display::InitWindow(int16_t width, int16_t height, const char *title)
{
    SDL_SetHint(SDL_HINT_TRACKPAD_IS_TOUCH_ONLY, "1");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

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
    cameraDirty = true;
}

void Display::Render()
{
    renderer.Render();
}

void Display::UpdateScene()
{
    sceneDirty = true;
}

void Display::Frame()
{
    if (!cameraDirty && !sceneDirty)
        return;

    if (cameraDirty)
    {
        renderer.SetCamera(camera);
        cameraDirty = false;
    }

    if (sceneDirty)
    {
        renderer.UpdateScene(scene);
        sceneDirty = false;
    }

    Render();
}

void Display::SetAspectRatio(const uint16_t width, const uint16_t height)
{
    glViewport(0, 0, width, height);
    camera.SetAspectRatio(static_cast<float>(width) / static_cast<float>(height));

    UpdateCamera();
}

void Display::Zoom(const float offsetY, const glm::vec3 &posCursotr)
{
    camera.Zoom(offsetY, posCursotr);

    UpdateCamera();
}

void Display::snapInput(float &x, float &y)
{
    if (std::abs(x) <= std::abs(y) * 0.5f)
        x = 0;
    else if (std::abs(y) <= std::abs(x) * 0.5f)
        y = 0;
}

void Display::Orbit(float offsetY, float offsetX)
{
    snapInput(offsetX, offsetY);
    camera.Orbit(offsetY, offsetX);

    UpdateCamera();
}

void Display::Roll(float delta)
{
    camera.Roll(delta);
    UpdateCamera();
}

void Display::Pan(float offsetX, float offsetY, bool scroll)
{
    snapInput(offsetX, offsetY);
    camera.Pan(offsetX, offsetY, scroll);

    UpdateCamera();
}