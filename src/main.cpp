#include <iostream>
#include <optional>

#include "scene.hpp"
#include "display/display.hpp"
#include "map"
#include "Analysis/Analysis.hpp"
#include "Analysis/Overhang/Overhang.hpp"
#include "Analysis/ThinSection/ThinSection.hpp"

Scene scene;
SDL_Window *window = nullptr;
std::optional<Display> display;

enum class RunType
{
    TEST,
    TERMINAL,
    WINDOW
};

struct Inputs
{
    double lastX = 0;
    double lastY = 0;
    bool shiftPressed = false;
    bool altPressed = false;
    bool rightPressed = false;
    bool middlePressed = false;
    bool scrolling = false;
};

Inputs input;

const RunType TEST = RunType::TEST;
const RunType TERMINAL = RunType::TERMINAL;
const RunType WINDOW = RunType::WINDOW;

bool forceRender = false;

void Process(bool renderUpdate, bool cameraUpdate, bool sceneUpdate, bool logic)
{
    bool active = false;
    if (cameraUpdate)
    {
        display->UpdateCamera();
        forceRender = true;
        active = true;
    }

    if (renderUpdate || forceRender)
    {
        if (sceneUpdate)
            display->UpdateBuffer(scene);

        display->Render(scene);
        forceRender = false;
        active = true;
    }

    if (active)
        LOG_BACK("Process loop ended");
}

glm::vec3 ProjectScreenToWorld(double mouseX, double mouseY,
                               int screenWidth, int screenHeight,
                               const Camera &camera)
{
    float ndcX = (2.0f * mouseX) / screenWidth - 1.0f;
    float ndcY = 1.0f - (2.0f * mouseY) / screenHeight;

    glm::mat4 view = camera.GetViewMatrix();
    glm::mat4 proj = camera.GetProjectionMatrix();
    glm::mat4 viewProj = proj * view;
    glm::mat4 invViewProj = glm::inverse(viewProj);

    glm::vec4 nearPoint = invViewProj * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    nearPoint /= nearPoint.w;

    return glm::vec3(nearPoint);
}

void HandleEvents(bool &running)
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_EVENT_QUIT:
            running = false;
            break;

        case SDL_EVENT_KEY_DOWN:
            if (event.key.repeat)
                break;
            if (event.key.key == SDLK_LSHIFT)
                input.shiftPressed = true;
            if (event.key.key == SDLK_LALT)
                input.altPressed = true;
            break;

        case SDL_EVENT_KEY_UP:
            if (event.key.key == SDLK_LSHIFT)
            {
                input.shiftPressed = false;
                input.scrolling = false;
            }
            if (event.key.key == SDLK_LALT)
            {
                input.altPressed = false;
                input.scrolling = false;
            }
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        {
            bool pressed = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            if (event.button.button == SDL_BUTTON_RIGHT)
                input.rightPressed = pressed;
            if (event.button.button == SDL_BUTTON_MIDDLE)
                input.middlePressed = pressed;
            break;
        }

        case SDL_EVENT_MOUSE_MOTION:
        {
            float deltaX = event.motion.xrel;
            float deltaY = event.motion.yrel;
            input.lastX = event.motion.x;
            input.lastY = event.motion.y;

            if (input.rightPressed)
            {
                display->Pan(deltaX, deltaY, false);
                Process(false, true, false, false);
            }
            if (input.middlePressed)
            {
                display->Orbit(deltaX, deltaY);
                Process(false, true, false, false);
            }
            break;
        }

        case SDL_EVENT_MOUSE_WHEEL:
        {
            float x = event.wheel.x;
            float y = event.wheel.y;

            if (input.shiftPressed)
            {
                int width, height;
                SDL_GetWindowSizeInPixels(window, &width, &height);

                glm::vec3 cursorPos = ProjectScreenToWorld(
                    input.lastX, input.lastY, width, height, display->GetCamera());

                display->Zoom(y, cursorPos);
            }
            else if (input.altPressed)
            {
                display->Orbit(x, y);
            }
            else
            {
                display->Pan(x, y);
            }

            Process(false, true, false, false);
            break;
        }

        case SDL_EVENT_WINDOW_RESIZED:
        {
            int width = event.window.data1;
            int height = event.window.data2;
            if (height > 0)
            {
                display->SetAspectRatio(width, height);
                Process(false, true, false, false);
            }
            break;
        }
        }
    }
}

void Shutdown()
{
    if (display)
        display->Shutdown();
}

bool Init()
{
    display.emplace(1280, 720, "CAD OpenGL");

    window = display->GetWindow();
    if (window == nullptr)
    {
        Shutdown();
        return LOG_FALSE("Window is null");
    }

    Analysis::Instance().AddFaceAnalysis(std::make_unique<Overhang>());
    Analysis::Instance().AddSolidAnalysis(std::make_unique<ThinSection>());

    return true;
}

RunType type = WINDOW;

int main()
{
    if (type == TEST)
    {
    }
    else if (type == TERMINAL)
    {
    }
    else if (type == WINDOW)
    {
        LOG_BACK("Running in window");

        if (!Init())
            return -1;

        Point *bp1 = scene.CreatePoint(glm::vec3(0, 0, 0));
        Point *bp2 = scene.CreatePoint(glm::vec3(0, 10, 0));
        Point *bp3 = scene.CreatePoint(glm::vec3(10, 10, 0));
        Point *bp4 = scene.CreatePoint(glm::vec3(10, 0, 0));

        Edge *be1 = scene.CreateEdge(bp1, bp2);
        Edge *be2 = scene.CreateEdge(bp2, bp3);
        Edge *be3 = scene.CreateEdge(bp3, bp4);
        Edge *be4 = scene.CreateEdge(bp4, bp1);

        Face *bf = scene.CreateFace({{be1, be2, be3, be4}});

        Point *ep = scene.CreatePoint(glm::vec3(0, 0, 10));

        Edge *e1 = scene.CreateEdge(bp1, ep);
        Edge *e2 = scene.CreateEdge(bp2, ep);
        Edge *e3 = scene.CreateEdge(bp3, ep);
        Edge *e4 = scene.CreateEdge(bp4, ep);

        Face *ef1 = scene.CreateFace({{be1, e2, e1}});
        Face *ef2 = scene.CreateFace({{be2, e3, e2}});
        Face *ef3 = scene.CreateFace({{be3, e4, e3}});
        Face *ef4 = scene.CreateFace({{be4, e1, e4}});

        Solid *s = scene.CreateSolid({bf, ef1, ef2, ef3, ef4});

        display->AddForm(s);

        Process(true, true, true, false);

        LOG_FILTER_BACK(true);
        bool running = true;
        while (running)
        {
            HandleEvents(running);
            Process(false, false, false, false);
        }

        Shutdown();
    }
    LOG_END
    return 0;
}
