#include <iostream>
#include <optional>
#include "map"

#include "scene.hpp"
#include "display/display.hpp"
#include "input/Input.hpp"

#include "Analysis/Analysis.hpp"
#include "Analysis/Overhang/Overhang.hpp"
#include "Analysis/ThinSection/ThinSection.hpp"
#include "Analysis/SharpCorner/SharpCorner.hpp"

#include "Analysis/SmallFeature/SmallFeature.hpp"

Scene scene;
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
        display->Shutdown();
}

bool Init()
{
    display.emplace(1280, 720, "CAD OpenGL", &scene);
    input.emplace(&display.value());
    window = display->GetWindow();
    if (window == nullptr)
    {
        Shutdown();
        return LOG_FALSE("Window is null");
    }

    Analysis::Instance().AddFaceAnalysis(std::make_unique<Overhang>());
    Analysis::Instance().AddSolidAnalysis(std::make_unique<ThinSection>());
    Analysis::Instance().AddEdgeAnalysis(std::make_unique<SharpCorner>());
    Analysis::Instance().AddSolidAnalysis(std::make_unique<SmallFeature>());

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
