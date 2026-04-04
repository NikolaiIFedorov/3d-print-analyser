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
#include "Analysis/Bridging/Bridging.hpp"
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
    Analysis::Instance().AddSolidAnalysis(std::make_unique<SharpCorner>());
    Analysis::Instance().AddSolidAnalysis(std::make_unique<Bridging>());
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

        display->UpdateScene();

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
