#include "display.hpp"
#include "rendering/color.hpp"
#include "logic/Analysis/Analysis.hpp"
#include "logic/Import/STLImport.hpp"
#include "logic/Import/OBJImport.hpp"
#include "logic/Import/ThreeMFImport.hpp"
#include "input/FileImport.hpp"

Display::Display(int16_t width, int16_t height, const char *title, Scene *scene) : window(InitWindow(width, height, title)), renderer(GetWindow()), analysisRenderer(GetWindow()), viewportRenderer(GetWindow()), uiRenderer(GetWindow()), camera(width, height), scene(scene)
{
    InitUI();
    SDL_AddEventWatch(ResizeEventWatcher, this);
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

bool Display::ResizeEventWatcher(void *userdata, SDL_Event *event)
{
    if (event->type == SDL_EVENT_WINDOW_RESIZED)
    {
        Display *self = static_cast<Display *>(userdata);
        int width = event->window.data1;
        int height = event->window.data2;
        if (height > 0)
        {
            self->SetAspectRatio(width, height);
            self->Render();
        }
    }
    return true;
}

void Display::Shutdown()
{
    SDL_RemoveEventWatch(ResizeEventWatcher, this);
    uiRenderer.Shutdown();
    analysisRenderer.Shutdown();
    viewportRenderer.Shutdown();
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
    glClearColor(BASE, BASE, BASE, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    viewportRenderer.Render();
    renderer.Render();
    analysisRenderer.Render();
    uiRenderer.Render();
    SDL_GL_SwapWindow(window);
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
        analysisRenderer.SetCamera(camera);
        viewportRenderer.SetCamera(camera);
        cameraDirty = false;
    }

    if (sceneDirty)
    {
        renderer.UpdateScene(scene);
        if (analysisEnabled)
        {
            auto results = Analysis::Instance().AnalyzeScene(scene);
            analysisRenderer.Update(scene, results);
        }
        else
        {
            analysisRenderer.Clear();
        }
        sceneDirty = false;
    }

    Render();
}

void Display::SetAspectRatio(const uint16_t width, const uint16_t height)
{
    glViewport(0, 0, width, height);
    camera.SetAspectRatio(static_cast<float>(width) / static_cast<float>(height));
    uiRenderer.SetScreenSize(width, height);

    UpdateCamera();
}

void Display::Zoom(const float offsetY, const glm::vec3 &posCursotr)
{
    camera.Zoom(offsetY, posCursotr);

    UpdateCamera();
}

glm::vec3 Display::ScreenToWorld(float pixelX, float pixelY) const
{
    int w, h;
    SDL_GetWindowSize(window, &w, &h);

    float ndcX = 2.0f * pixelX / w - 1.0f;
    float ndcY = 1.0f - 2.0f * pixelY / h;

    glm::vec3 right = camera.orientation * glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 up = camera.orientation * glm::vec3(0.0f, 1.0f, 0.0f);

    return camera.target + right * ndcX * camera.orthoSize * camera.aspectRatio + up * ndcY * camera.orthoSize;
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

void Display::FrameScene()
{
    if (scene->points.empty())
        return;

    glm::vec3 min(std::numeric_limits<float>::max());
    glm::vec3 max(std::numeric_limits<float>::lowest());

    for (const auto &point : scene->points)
    {
        glm::vec3 pos(point.position);
        min = glm::min(min, pos);
        max = glm::max(max, pos);
    }

    camera.FrameBounds(min, max);
    UpdateCamera();
}

bool Display::HitTestUI(float pixelX, float pixelY) const
{
    return uiRenderer.HitTest(pixelX, pixelY);
}

bool Display::HandleClick(float pixelX, float pixelY)
{
    return uiRenderer.HandleClick(pixelX, pixelY);
}

void Display::InitUI()
{
    float toolsWidth = 2.0f;  // sidebar width in cells
    float sceneWidth = 10.0f; // sidebar width in cells
    float filesWidth = 2.5f;  // top bar height in cells

    const auto &grid = uiRenderer.GetGrid();

    Panel analysisDef;
    analysisDef.id = "analysis";
    analysisDef.color = Color::GetUI(1);
    analysisDef.leftAnchor = PanelAnchor{nullptr, PanelAnchor::Left};
    analysisDef.width = toolsWidth + UIGrid::GAP + sceneWidth;
    analysisDef.topAnchor = PanelAnchor{nullptr, PanelAnchor::Top};
    analysisDef.height = filesWidth;
    Panel &analysis = uiRenderer.AddPanel(analysisDef);
    uiRenderer.AddButton(analysis, [this]()
                         {
                             analysisEnabled = !analysisEnabled;
                             UpdateScene(); });

    Panel toolsDef;
    toolsDef.id = "tools";
    toolsDef.color = Color::GetUI(1);
    toolsDef.leftAnchor = PanelAnchor{nullptr, PanelAnchor::Left};
    toolsDef.width = toolsWidth;
    toolsDef.topAnchor = PanelAnchor{&analysis, PanelAnchor::Bottom};
    toolsDef.bottomAnchor = PanelAnchor{nullptr, PanelAnchor::Bottom};
    Panel &tools = uiRenderer.AddPanel(toolsDef);

    Panel sceneDef;
    sceneDef.id = "scene";
    sceneDef.color = Color::GetUI(1);
    sceneDef.leftAnchor = PanelAnchor{&tools, PanelAnchor::Right};
    sceneDef.width = sceneWidth;
    sceneDef.topAnchor = PanelAnchor{&analysis, PanelAnchor::Bottom};
    sceneDef.bottomAnchor = PanelAnchor{nullptr, PanelAnchor::Bottom};
    Panel &scene = uiRenderer.AddPanel(sceneDef);

    Panel filesDef;
    filesDef.id = "files";
    filesDef.color = Color::GetUI(1);
    filesDef.leftAnchor = PanelAnchor{&scene, PanelAnchor::Right};
    filesDef.rightAnchor = PanelAnchor{nullptr, PanelAnchor::Right};
    filesDef.topAnchor = PanelAnchor{nullptr, PanelAnchor::Top};
    filesDef.height = filesWidth;
    uiRenderer.AddPanel(filesDef);
    uiRenderer.AddButton(filesDef, [this]()
                         { FileImport::OpenFileDialog(window, [this](const std::string &path)
                                                      {
                                                         auto ext = path.substr(path.find_last_of('.') + 1);
                                                         std::string lower;
                                                         for (char c : ext) lower += std::tolower(c);

                                                         if (lower == "stl")
                                                             STLImport::Import(path, this->scene);
                                                         else if (lower == "obj")
                                                             OBJImport::Import(path, this->scene);
                                                         else if (lower == "3mf")
                                                             ThreeMFImport::Import(path, this->scene);

                                                         FrameScene();
                                                         UpdateScene(); }); });
}