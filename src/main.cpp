#include <iostream>

#include "scene.hpp"
#include "display/display.hpp"

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

    glm::vec4 nearPoint = invViewProj * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
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

    glm::vec3 cursorPos = ProjectScreenToWorld(
        input.lastX, input.lastY, width, height, display.GetCamera());

    if (input.shiftPressed)
    {
        display.Zoom(yoffset, cursorPos);
    }
    if (input.altPressed)
    {
        display.Orbit(xoffset, yoffset);
    }

    if (!input.shiftPressed && !input.altPressed)
    {
        display.Pan(xoffset, yoffset);
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

    glfwSetCursorPosCallback(window, cursorPositionCallback);
    glfwSetScrollCallback(window, scrollCallback);
    glfwSetKeyCallback(window, keyPressCallback);
    glfwSetFramebufferSizeCallback(window, windowResizeCallback);

    return true;
}

RunType type = WINDOW;

int main()
{
    LOG_START
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

        uint32_t p1 = scene.CreatePoint({1.0f, 0.0f, 0.0f});
        uint32_t p2 = scene.CreatePoint({1.0f, 1.0f, 0.0f});
        uint32_t p3 = scene.CreatePoint({0.0f, 1.0f, 0.0f});
        uint32_t p4 = scene.CreatePoint({0.0f, 0.0f, 0.0f});

        uint32_t e1 = scene.CreateLineEdge(p1, p2);
        uint32_t e2 = scene.CreateLineEdge(p2, p3);
        uint32_t e3 = scene.CreateLineEdge(p3, p4);
        uint32_t e4 = scene.CreateLineEdge(p4, p1);

        uint32_t f1 = scene.CreatePlanarFace({{e1, e2, e3, e4}});

        display.AddForm(f1);

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
