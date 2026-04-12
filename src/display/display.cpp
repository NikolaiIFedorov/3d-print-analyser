#include "display.hpp"
#include "rendering/color.hpp"
#include "logic/Analysis/Analysis.hpp"
#include "logic/Analysis/Overhang/Overhang.hpp"
#include "logic/Analysis/SharpCorner/SharpCorner.hpp"
#include "logic/Analysis/SmallFeature/SmallFeature.hpp"
#include "logic/Import/STLImport.hpp"

#include <unordered_set>
#include <queue>
#include <cstdio>
#include "logic/Import/OBJImport.hpp"
#include "logic/Import/ThreeMFImport.hpp"
#include "input/FileImport.hpp"

Display::Display(int16_t width, int16_t height, const char *title, Scene *scene) : window(InitWindow(width, height, title)), renderer(GetWindow()), analysisRenderer(GetWindow()), viewportRenderer(GetWindow()), uiRenderer(GetWindow(), "/System/Library/Fonts/Helvetica.ttc"), camera(width, height), scene(scene)
{
    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForOpenGL(window, glContext);
    ImGui_ImplOpenGL3_Init("#version 330");

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
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

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

void Display::RebuildAnalysis()
{
    Analysis::Instance().Clear();
    Analysis::Instance().AddFaceAnalysis(std::make_unique<Overhang>(overhangAngle));
    auto sharpCorner = std::make_unique<SharpCorner>(sharpCornerAngle);
    const SharpCorner *sharpCornerPtr = sharpCorner.get();
    Analysis::Instance().AddSolidAnalysis(std::make_unique<SmallFeature>(layerHeight, minFeatureSize, 3.0, sharpCornerPtr));
    Analysis::Instance().AddEdgeAnalysis(std::move(sharpCorner));
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

    // Start ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    uiRenderer.Render();

    // Finish ImGui frame
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    SDL_GL_SwapWindow(window);
}

void Display::UpdateScene()
{
    sceneDirty = true;
}

void Display::Frame()
{
    if (cameraDirty)
    {
        renderer.SetCamera(camera);
        analysisRenderer.SetCamera(camera);
        viewportRenderer.SetCamera(camera);
        cameraDirty = false;
    }

    if (sceneDirty)
    {
        bool hasModel = !scene->solids.empty() || !scene->faces.empty();

        // Toggle Analysis panel sections based on model presence
        uiRenderer.SetSectionVisible("Analysis", "Import file", !hasModel);
        uiRenderer.SetSectionVisible("Analysis", "Result", hasModel);
        uiRenderer.SetSectionVisible("Analysis", "Verdict", hasModel);
        uiRenderer.SetSectionVisible("Analysis", "Config", hasModel);

        AnalysisResults results;
        if (analysisEnabled)
            results = Analysis::Instance().AnalyzeScene(scene);

        renderer.UpdateScene(scene, analysisEnabled ? &results : nullptr);
        if (analysisEnabled)
        {
            analysisRenderer.Update(scene, results);

            // Count flaws per type and push to UI
            size_t thinSections = 0, smallFeatures = 0, sharpEdges = 0;

            // Count overhang regions as connected components of adjacent overhang faces
            // Skip defunct faces (loops cleared by MergeCoplanarFaces)
            std::unordered_set<const Face *> overhangFaces;
            for (const auto &[face, kind] : results.faceFlaws)
            {
                if (kind == FaceFlawKind::OVERHANG && !face->loops.empty())
                    overhangFaces.insert(face);
            }

            size_t overhangs = 0;
            std::unordered_set<const Face *> visited;
            for (const Face *seed : overhangFaces)
            {
                if (visited.count(seed))
                    continue;
                overhangs++;
                std::queue<const Face *> bfs;
                bfs.push(seed);
                visited.insert(seed);
                while (!bfs.empty())
                {
                    const Face *current = bfs.front();
                    bfs.pop();
                    for (const auto &loop : current->loops)
                    {
                        for (const auto &oe : loop)
                        {
                            for (Face *neighbor : oe.edge->dependencies)
                            {
                                if (overhangFaces.count(neighbor) && !visited.count(neighbor))
                                {
                                    visited.insert(neighbor);
                                    bfs.push(neighbor);
                                }
                            }
                        }
                    }
                }
            }

            for (const auto &[solid, flaws] : results.faceFlawRanges)
            {
                for (const auto &ff : flaws)
                {
                    switch (ff.flaw)
                    {
                    case FaceFlawKind::THIN_SECTION:
                        thinSections++;
                        break;
                    case FaceFlawKind::SMALL_FEATURE:
                        smallFeatures++;
                        break;
                    default:
                        break;
                    }
                }
            }
            for (const auto &[solid, edgeVec] : results.edgeFlaws)
            {
                for (const auto &e : edgeVec)
                {
                    if (e.flaw == EdgeFlawKind::SHARP_CORNER)
                        sharpEdges++;
                }
            }

            // Bright versions of flaw colors for UI text
            glm::vec4 overhangColor = glm::vec4(Color::GetFace(FaceFlawKind::OVERHANG).r + 0.4f,
                                                Color::GetFace(FaceFlawKind::OVERHANG).g + 0.2f,
                                                Color::GetFace(FaceFlawKind::OVERHANG).b + 0.2f, 1.0f);
            glm::vec4 thinColor = glm::vec4(Color::GetFace(FaceFlawKind::THIN_SECTION).r + 0.4f,
                                            Color::GetFace(FaceFlawKind::THIN_SECTION).g + 0.4f,
                                            Color::GetFace(FaceFlawKind::THIN_SECTION).b + 0.2f, 1.0f);
            glm::vec4 edgeColor = glm::vec4(Color::GetEdge(EdgeFlawKind::SHARP_CORNER).r + 0.3f,
                                            Color::GetEdge(EdgeFlawKind::SHARP_CORNER).g + 0.1f,
                                            Color::GetEdge(EdgeFlawKind::SHARP_CORNER).b + 0.1f, 1.0f);

            std::vector<SectionLine> lines;
            if (overhangs > 0)
                lines.push_back({std::to_string(overhangs), " overhang" + std::string(overhangs > 1 ? "s" : ""), overhangColor});
            if (thinSections > 0)
                lines.push_back({std::to_string(thinSections), " thin section" + std::string(thinSections > 1 ? "s" : ""), thinColor});
            if (smallFeatures > 0)
                lines.push_back({std::to_string(smallFeatures), " small feature" + std::string(smallFeatures > 1 ? "s" : ""), thinColor});
            if (sharpEdges > 0)
                lines.push_back({std::to_string(sharpEdges), " sharp edge" + std::string(sharpEdges > 1 ? "s" : ""), edgeColor});
            if (lines.empty())
                lines.push_back({"", "No flaws", Color::GetUIText(1)});

            uiRenderer.SetSectionValue("Analysis", "Result", lines);
        }
        else
        {
            analysisRenderer.Clear();
            uiRenderer.SetSectionValue("Analysis", "Result", {});
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

void Display::InitUI()
{
    float sidebarWidth = 10.0f;

    // Header
    Panel filesDef;
    filesDef.id = "Files";
    filesDef.color = Color::GetUI(1);
    filesDef.leftAnchor = PanelAnchor{nullptr, PanelAnchor::Left};
    filesDef.rightAnchor = PanelAnchor{nullptr, PanelAnchor::Right};
    filesDef.topAnchor = PanelAnchor{nullptr, PanelAnchor::Top};
    filesDef.minWidth = sidebarWidth;
    Panel &files = uiRenderer.AddPanel(filesDef);

    // Analysis panel with sections
    Panel analysisDef;
    analysisDef.id = "Analysis";
    analysisDef.color = Color::GetUI(1);
    analysisDef.leftAnchor = PanelAnchor{nullptr, PanelAnchor::Left};
    analysisDef.topAnchor = PanelAnchor{&files, PanelAnchor::Bottom};
    Panel &analysis = uiRenderer.AddPanel(analysisDef);
    Panel &result = analysis.AddSection("Result");
    result.showLabel = false;
    result.visible = false;
    Panel &import = analysis.AddSection("Import");
    import.id = "Import file";
    import.imguiContent = [this](float w, float h)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, h * 0.3f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.22f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.28f, 0.28f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
        if (ImGui::Button("Import file", ImVec2(w, h)))
        {
            FileImport::OpenFileDialog(window, [this](const std::string &path)
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
                UpdateScene(); });
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
    };
    analysis.AddSection("Verdict").visible = false;

    // Parameter group
    Panel &config = analysis.AddSection("Config");
    config.showLabel = false;
    config.visible = false;

    Panel &overhangContent = config.AddContent("Overhang angle (deg)");
    overhangContent.imguiContent = [this](float w, float h)
    {
        static bool requestEdit = false, editing = false, tracking = false;
        static ImVec2 startPos;
        glm::vec4 c = glm::vec4(Color::GetFace(FaceFlawKind::OVERHANG).r + 0.4f,
                                Color::GetFace(FaceFlawKind::OVERHANG).g + 0.2f,
                                Color::GetFace(FaceFlawKind::OVERHANG).b + 0.2f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c.r, c.g, c.b, c.a));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.15f, 0.15f, 0.15f, 0.5f));
        if (requestEdit)
        {
            ImGui::SetKeyboardFocusHere();
            requestEdit = false;
            editing = true;
        }
        float labelW = ImGui::CalcTextSize("Overhang:  ").x;
        float normalPad = ImGui::GetStyle().FramePadding.x;
        if (editing)
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(labelW + normalPad, ImGui::GetStyle().FramePadding.y));
        ImGui::SetNextItemWidth(w);
        bool changed = ImGui::DragFloat("##overhang", &overhangAngle, 0.5f, 0.0f, 90.0f,
                                        editing ? "%.0f" : "");
        if (editing)
            ImGui::PopStyleVar();
        editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
        if (ImGui::IsItemActivated())
        {
            tracking = true;
            startPos = ImGui::GetIO().MousePos;
        }
        if (tracking && !ImGui::IsItemActive())
        {
            ImVec2 ep = ImGui::GetIO().MousePos;
            float d = (ep.x - startPos.x) * (ep.x - startPos.x) + (ep.y - startPos.y) * (ep.y - startPos.y);
            if (d < 9.0f)
            {
                requestEdit = true;
                SDL_Event wake; SDL_zero(wake); wake.type = SDL_EVENT_USER; SDL_PushEvent(&wake);
            }
            tracking = false;
        }
        ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        float ty = mn.y + ImGui::GetStyle().FramePadding.y;
        ImU32 col = ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, c.a));
        ImGui::GetWindowDrawList()->AddText(ImVec2(mn.x + normalPad, ty), col, "Overhang:");
        if (!editing)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0f\u00b0", overhangAngle);
            ImVec2 vs = ImGui::CalcTextSize(buf);
            ImGui::GetWindowDrawList()->AddText(ImVec2(mx.x - normalPad - vs.x, ty), col, buf);
        }
        if (changed)
        {
            RebuildAnalysis();
            UpdateScene();
        }
        ImGui::PopStyleColor(4);
    };

    Panel &sharpContent = config.AddContent("Sharp corner (deg)");
    sharpContent.imguiContent = [this](float w, float h)
    {
        static bool requestEdit = false, editing = false, tracking = false;
        static ImVec2 startPos;
        glm::vec4 c = glm::vec4(Color::GetEdge(EdgeFlawKind::SHARP_CORNER).r + 0.3f,
                                Color::GetEdge(EdgeFlawKind::SHARP_CORNER).g + 0.1f,
                                Color::GetEdge(EdgeFlawKind::SHARP_CORNER).b + 0.1f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c.r, c.g, c.b, c.a));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.15f, 0.15f, 0.15f, 0.5f));
        if (requestEdit)
        {
            ImGui::SetKeyboardFocusHere();
            requestEdit = false;
            editing = true;
        }
        float labelW = ImGui::CalcTextSize("Sharp corner:  ").x;
        float normalPad = ImGui::GetStyle().FramePadding.x;
        if (editing)
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(labelW + normalPad, ImGui::GetStyle().FramePadding.y));
        ImGui::SetNextItemWidth(w);
        bool changed = ImGui::DragFloat("##sharp", &sharpCornerAngle, 0.5f, 0.0f, 180.0f,
                                        editing ? "%.0f" : "");
        if (editing)
            ImGui::PopStyleVar();
        editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
        if (ImGui::IsItemActivated())
        {
            tracking = true;
            startPos = ImGui::GetIO().MousePos;
        }
        if (tracking && !ImGui::IsItemActive())
        {
            ImVec2 ep = ImGui::GetIO().MousePos;
            float d = (ep.x - startPos.x) * (ep.x - startPos.x) + (ep.y - startPos.y) * (ep.y - startPos.y);
            if (d < 9.0f)
            {
                requestEdit = true;
                SDL_Event wake; SDL_zero(wake); wake.type = SDL_EVENT_USER; SDL_PushEvent(&wake);
            }
            tracking = false;
        }
        ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        float ty = mn.y + ImGui::GetStyle().FramePadding.y;
        ImU32 col = ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, c.a));
        ImGui::GetWindowDrawList()->AddText(ImVec2(mn.x + normalPad, ty), col, "Sharp corner:");
        if (!editing)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0f\u00b0", sharpCornerAngle);
            ImVec2 vs = ImGui::CalcTextSize(buf);
            ImGui::GetWindowDrawList()->AddText(ImVec2(mx.x - normalPad - vs.x, ty), col, buf);
        }
        if (changed)
        {
            RebuildAnalysis();
            UpdateScene();
        }
        ImGui::PopStyleColor(4);
    };

    Panel &featureContent = config.AddContent("Min feature (mm)");
    featureContent.imguiContent = [this](float w, float h)
    {
        static bool requestEdit = false, editing = false, tracking = false;
        static ImVec2 startPos;
        glm::vec4 c = glm::vec4(Color::GetFace(FaceFlawKind::THIN_SECTION).r + 0.4f,
                                Color::GetFace(FaceFlawKind::THIN_SECTION).g + 0.4f,
                                Color::GetFace(FaceFlawKind::THIN_SECTION).b + 0.2f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c.r, c.g, c.b, c.a));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.15f, 0.15f, 0.15f, 0.5f));
        if (requestEdit)
        {
            ImGui::SetKeyboardFocusHere();
            requestEdit = false;
            editing = true;
        }
        float labelW = ImGui::CalcTextSize("Min feature:  ").x;
        float normalPad = ImGui::GetStyle().FramePadding.x;
        if (editing)
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(labelW + normalPad, ImGui::GetStyle().FramePadding.y));
        ImGui::SetNextItemWidth(w);
        bool changed = ImGui::DragFloat("##feature", &minFeatureSize, 0.05f, 0.1f, 50.0f,
                                        editing ? "%.1f" : "");
        if (editing)
            ImGui::PopStyleVar();
        editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
        if (ImGui::IsItemActivated())
        {
            tracking = true;
            startPos = ImGui::GetIO().MousePos;
        }
        if (tracking && !ImGui::IsItemActive())
        {
            ImVec2 ep = ImGui::GetIO().MousePos;
            float d = (ep.x - startPos.x) * (ep.x - startPos.x) + (ep.y - startPos.y) * (ep.y - startPos.y);
            if (d < 9.0f)
            {
                requestEdit = true;
                SDL_Event wake; SDL_zero(wake); wake.type = SDL_EVENT_USER; SDL_PushEvent(&wake);
            }
            tracking = false;
        }
        ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        float ty = mn.y + ImGui::GetStyle().FramePadding.y;
        ImU32 col = ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, c.a));
        ImGui::GetWindowDrawList()->AddText(ImVec2(mn.x + normalPad, ty), col, "Min feature:");
        if (!editing)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f mm", minFeatureSize);
            ImVec2 vs = ImGui::CalcTextSize(buf);
            ImGui::GetWindowDrawList()->AddText(ImVec2(mx.x - normalPad - vs.x, ty), col, buf);
        }
        if (changed)
        {
            RebuildAnalysis();
            UpdateScene();
        }
        ImGui::PopStyleColor(4);
    };

    Panel &layerContent = config.AddContent("Layer height (mm)");
    layerContent.imguiContent = [this](float w, float h)
    {
        static bool requestEdit = false, editing = false, tracking = false;
        static ImVec2 startPos;
        glm::vec4 c = Color::GetUIText(1);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c.r, c.g, c.b, c.a));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.15f, 0.15f, 0.15f, 0.5f));
        if (requestEdit)
        {
            ImGui::SetKeyboardFocusHere();
            requestEdit = false;
            editing = true;
        }
        float labelW = ImGui::CalcTextSize("Layer height:  ").x;
        float normalPad = ImGui::GetStyle().FramePadding.x;
        if (editing)
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(labelW + normalPad, ImGui::GetStyle().FramePadding.y));
        ImGui::SetNextItemWidth(w);
        bool changed = ImGui::DragFloat("##layer", &layerHeight, 0.01f, 0.01f, 5.0f,
                                        editing ? "%.2f" : "");
        if (editing)
            ImGui::PopStyleVar();
        editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
        if (ImGui::IsItemActivated())
        {
            tracking = true;
            startPos = ImGui::GetIO().MousePos;
        }
        if (tracking && !ImGui::IsItemActive())
        {
            ImVec2 ep = ImGui::GetIO().MousePos;
            float d = (ep.x - startPos.x) * (ep.x - startPos.x) + (ep.y - startPos.y) * (ep.y - startPos.y);
            if (d < 9.0f)
            {
                requestEdit = true;
                SDL_Event wake; SDL_zero(wake); wake.type = SDL_EVENT_USER; SDL_PushEvent(&wake);
            }
            tracking = false;
        }
        ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        float ty = mn.y + ImGui::GetStyle().FramePadding.y;
        ImU32 col = ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, c.a));
        ImGui::GetWindowDrawList()->AddText(ImVec2(mn.x + normalPad, ty), col, "Layer height:");
        if (!editing)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.2f mm", layerHeight);
            ImVec2 vs = ImGui::CalcTextSize(buf);
            ImGui::GetWindowDrawList()->AddText(ImVec2(mx.x - normalPad - vs.x, ty), col, buf);
        }
        if (changed)
        {
            RebuildAnalysis();
            UpdateScene();
        }
        ImGui::PopStyleColor(4);
    };

    RebuildAnalysis();

    // Compute minimum grid extent and enforce as SDL minimum window size
    uiRenderer.ComputeMinGridSize();
    const auto &grid = uiRenderer.GetGrid();
    SDL_SetWindowMinimumSize(window, grid.MinWidthPixels(), grid.MinHeightPixels());
}