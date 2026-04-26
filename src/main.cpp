#include <iostream>
#include <optional>
#include "map"

#include "scene.hpp"
#include "display/display.hpp"
#include "input/Input.hpp"
#include "utils/SessionLogger.hpp"

SDL_Window *window = nullptr;
std::optional<Display> display;
std::optional<Input> input;

enum class RunType
{
    TEST,
    TERMINAL,
    WINDOW
};

const RunType TEST = RunType::TEST;
const RunType TERMINAL = RunType::TERMINAL;
const RunType WINDOW = RunType::WINDOW;

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

void Shutdown()
{
    if (display)
        display->FillSessionReproState(SessionLogger::Instance().state);
    SessionLogger::Instance().LogSessionEndSnapshot();
    SessionLogger::Instance().Flush("session_log.json");
    if (display)
        display->Shutdown();
}

bool Init()
{
    SessionLogger::Instance().Start();
    display.emplace(1280, 720, "CAD OpenGL");
    input.emplace(&display.value());
    window = display->GetWindow();
    if (window == nullptr)
    {
        Shutdown();
        return LOG_FALSE("Window is null");
    }

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

        LOG_FILTER_BACK(true);
        display->Frame();
        while (input->handleEvents())
        {
            display->Frame();
        }

        Shutdown();
    }
    LOG_END
    return 0;
}
