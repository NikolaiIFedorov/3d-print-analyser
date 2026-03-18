#include <iostream>

#include "scene.hpp"
#include "display/display.hpp"
#include "map"
#include "Analysis/Analysis.hpp"
#include "Analysis/Overhang/Overhang.hpp"
#include "Analysis/ThinSection/ThinSection.hpp"

Scene scene;

Display display(1280, 860, "CAD");
GLFWwindow *window = display.GetWindow();

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
        display.UpdateCamera();
        forceRender = true;
        active = true;
    }

    if (renderUpdate || forceRender)
    {
        if (sceneUpdate)
            display.UpdateBuffer(scene);

        display.Render(scene);
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

void keyPressCallback(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    if (action == GLFW_REPEAT)
        return;

    if (action == GLFW_PRESS)
    {
        if (key == GLFW_KEY_LEFT_SHIFT)
        {
            input.shiftPressed = true;
        }
        if (key == GLFW_KEY_LEFT_ALT)
            input.altPressed = true;
    }
    else if (action == GLFW_RELEASE)
    {
        if (key == GLFW_KEY_LEFT_SHIFT)
        {
            input.shiftPressed = false;
            input.scrolling = false;
        }
        if (key == GLFW_KEY_LEFT_ALT)
        {
            input.altPressed = false;
            input.scrolling = false;
        }
    }
    Process(false, false, false, false);
}

void cursorPositionCallback(GLFWwindow *window, double xpos, double ypos)
{
    double deltaX = xpos - input.lastX;
    double deltaY = ypos - input.lastY;

    input.lastX = xpos;
    input.lastY = ypos;
    Process(false, false, false, false);
}

void scrollCallback(GLFWwindow *window, double xoffset, double yoffset)
{
    int width, height;
    glfwGetFramebufferSize(window, &width, &height);

    LOG_DEBU("Scroll input: xoffset = " + Log::NumToStr(xoffset) +
             ", yoffset = " + Log::NumToStr(yoffset));

    double y = yoffset;
    double x = xoffset;

    if (std::abs(yoffset) <= std::abs(xoffset) * 2)
        y = 0;
    else if (std::abs(xoffset) <= std::abs(yoffset) * 2)
        x = 0;

    glm::vec3 cursorPos = ProjectScreenToWorld(
        input.lastX, input.lastY, width, height, display.GetCamera());

    if (input.shiftPressed)
    {
        display.Zoom(y, cursorPos);
    }
    if (input.altPressed)
    {
        display.Orbit(x, y);
    }

    if (!input.shiftPressed && !input.altPressed)
    {
        display.Pan(x, y);
    }

    Process(false, true, false, false);
}

void windowResizeCallback(GLFWwindow *window, int width, int height)
{
    if (height > 0)
    {
        display.SetAspectRatio(width, height);
        Process(false, true, false, false);
    }
}

void Shutdown()
{
    display.Shutdown();
}

bool Init()
{
    if (window == nullptr)
    {
        Shutdown();
        return LOG_FALSE("Window is null");
    }

    Analysis::Instance().AddFaceAnalysis(std::make_unique<Overhang>());
    Analysis::Instance().AddSolidAnalysis(std::make_unique<ThinSection>());

    glfwSetCursorPosCallback(window, cursorPositionCallback);
    glfwSetScrollCallback(window, scrollCallback);
    glfwSetKeyCallback(window, keyPressCallback);
    glfwSetFramebufferSizeCallback(window, windowResizeCallback);

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

        display.AddForm(s);

        Process(true, true, true, false);

        LOG_FILTER_BACK(true);
        while (!glfwWindowShouldClose(window))
        {
            glfwPollEvents();
            Process(false, false, false, false);
        }

        Shutdown();
    }
    LOG_END
    return 0;
}
