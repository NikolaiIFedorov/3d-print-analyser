#include <cstdio>
#include <iostream>
#include <optional>
#include "map"

#include "scene.hpp"
#include "ProjectionDepthMode.hpp"
#include "display/display.hpp"
#include "input/Input.hpp"
#include "utils/log.hpp"
#include "utils/SessionLogger.hpp"
#include "utils/ShutdownStackTrace.hpp"
#include "utils/CrashBacktrace.hpp"
#include <chrono>

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
    glm::mat4 proj = ProjectionDepthMode::EffectiveProjection(camera.GetProjectionMatrix());
    glm::mat4 viewProj = proj * view;
    glm::mat4 invViewProj = glm::inverse(viewProj);

    glm::vec4 nearPoint = invViewProj * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    nearPoint /= nearPoint.w;

    return glm::vec3(nearPoint);
}

void Shutdown()
{
    ShutdownStackTraceLogIfEnabled("mainthread main::Shutdown entry");
    SessionLogger &sl = SessionLogger::Instance();
    sl.LogShutdownPhase("main: begin");
    if (display)
    {
        display->FillSessionReproState(sl.state);
        sl.LogShutdownPhase("main: after FillSessionReproState");
    }
    sl.LogSessionEndSnapshot(false);
    sl.LogShutdownPhase("main: after LogSessionEndSnapshot");
    if (display)
    {
        sl.LogShutdownPhase("main: before Display::Shutdown");
        ShutdownStackTraceLogIfEnabled("mainthread before Display::Shutdown");
        display->Shutdown();
        ShutdownStackTraceLogIfEnabled("mainthread after Display::Shutdown");
        sl.LogShutdownPhase("main: after Display::Shutdown");
    }
    sl.LogShutdownPhase("main: end");
    ShutdownStackTraceLogIfEnabled("mainthread before session log flush");
    sl.Flush("session_log.json", false);
}

bool Init()
{
    SessionLogger::Instance().Start();
    display.emplace(1280, 720, "CAD OpenGL");
    input.emplace(&display.value());
    display->SetInput(&input.value());
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
    setvbuf(stdout, nullptr, _IOLBF, 0);
    InstallCrashBacktraceIfEnabled();
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

        Log::SetVerbosity(LogVerbosity::NORMAL);
        display->Frame();
        // Diagnostic only: flags real idle pauses (mouse/input stayed quiet) so we can correlate
        // them against the next render's timing and check whether a GPU power-state wake stall
        // (driver clocks down during idle, first GL call after resuming stalls) explains the
        // occasional one-off ui_render spike seen in profiling.
        auto lastFrameAt = std::chrono::steady_clock::now();
        while (input->handleEvents())
        {
            const auto now = std::chrono::steady_clock::now();
            const double idleGapMs = std::chrono::duration<double, std::milli>(now - lastFrameAt).count();
            if (idleGapMs >= 500.0)
                LOG_SESSION("Render stage", "idle_gap", "ms", idleGapMs);
            display->Frame();
            lastFrameAt = std::chrono::steady_clock::now();
        }

        Shutdown();
    }
    LOG_END
    return 0;
}
