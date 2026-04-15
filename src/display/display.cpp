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
#include <random>
#include "logic/Import/OBJImport.hpp"
#include "logic/Import/ThreeMFImport.hpp"
#include "input/FileImport.hpp"

Display::Display(int16_t width, int16_t height, const char *title, Scene *scene) : window(InitWindow(width, height, title)), renderer(GetWindow()), analysisRenderer(GetWindow()), viewportRenderer(GetWindow()), uiRenderer(GetWindow(), "/System/Library/Fonts/SFNS.ttf"), camera(width, height), scene(scene)
{
    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Helvetica.ttc", 16.0f);
    ImFont *pixelFont = io.Fonts->AddFontDefault();
    ImGui_ImplSDL3_InitForOpenGL(window, glContext);
    ImGui_ImplOpenGL3_Init("#version 330");
    uiRenderer.SetPixelImFont(pixelFont);

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
    renderDirty = true;
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
    renderDirty = true;
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
        if (uiImportPara)
            uiImportPara->visible = !hasModel;
        if (uiResult)
            uiResult->visible = hasModel;
        if (uiVerdict)
            uiVerdict->visible = hasModel;
        if (uiConfig)
            uiConfig->visible = hasModel;
        uiRenderer.MarkDirty();

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

            // Compute 3D bounding boxes per flaw category for click-to-frame
            auto expandBounds = [](glm::vec3 &bMin, glm::vec3 &bMax, const glm::dvec3 &p)
            {
                glm::vec3 fp(p);
                bMin = glm::min(bMin, fp);
                bMax = glm::max(bMax, fp);
            };
            auto expandFaceBounds = [&](glm::vec3 &bMin, glm::vec3 &bMax, const Face *face)
            {
                for (const auto &loop : face->loops)
                    for (const auto &oe : loop)
                    {
                        expandBounds(bMin, bMax, oe.edge->startPoint->position);
                        expandBounds(bMin, bMax, oe.edge->endPoint->position);
                    }
            };

            constexpr float INF = std::numeric_limits<float>::max();
            glm::vec3 overhangMin(INF), overhangMax(-INF);
            glm::vec3 thinMin(INF), thinMax(-INF);
            glm::vec3 smallMin(INF), smallMax(-INF);
            glm::vec3 sharpMin(INF), sharpMax(-INF);

            // Overhang face bounds
            for (const Face *face : overhangFaces)
                expandFaceBounds(overhangMin, overhangMax, face);

            // Thin section / small feature bounds from faceFlawRanges
            for (const auto &[solid, flaws] : results.faceFlawRanges)
            {
                for (const auto &ff : flaws)
                {
                    if (ff.flaw == FaceFlawKind::THIN_SECTION && ff.face)
                        expandFaceBounds(thinMin, thinMax, ff.face);
                    else if (ff.flaw == FaceFlawKind::SMALL_FEATURE && ff.face)
                        expandFaceBounds(smallMin, smallMax, ff.face);
                }
            }

            // Sharp edge bounds
            for (const auto &[solid, edgeVec] : results.edgeFlaws)
            {
                for (const auto &e : edgeVec)
                {
                    if (e.flaw == EdgeFlawKind::SHARP_CORNER && e.edge)
                    {
                        expandBounds(sharpMin, sharpMax, e.edge->startPoint->position);
                        expandBounds(sharpMin, sharpMax, e.edge->endPoint->position);
                    }
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

            auto makeFrameCallback = [this](glm::vec3 bMin, glm::vec3 bMax) -> std::function<void()>
            {
                if (bMin.x > bMax.x)
                    return nullptr; // no valid bounds
                return [this, bMin, bMax]()
                {
                    camera.FrameBounds(bMin, bMax);
                    cameraDirty = true;
                    renderDirty = true;
                };
            };

            std::vector<SectionLine> lines;
            if (overhangs > 0)
                lines.push_back({std::to_string(overhangs), " overhang" + std::string(overhangs > 1 ? "s" : ""), overhangColor, makeFrameCallback(overhangMin, overhangMax)});
            if (thinSections > 0)
                lines.push_back({std::to_string(thinSections), " thin section" + std::string(thinSections > 1 ? "s" : ""), thinColor, makeFrameCallback(thinMin, thinMax)});
            if (smallFeatures > 0)
                lines.push_back({std::to_string(smallFeatures), " small feature" + std::string(smallFeatures > 1 ? "s" : ""), thinColor, makeFrameCallback(smallMin, smallMax)});
            if (sharpEdges > 0)
                lines.push_back({std::to_string(sharpEdges), " sharp edge" + std::string(sharpEdges > 1 ? "s" : ""), edgeColor, makeFrameCallback(sharpMin, sharpMax)});
            if (uiResult)
            {
                uiResult->visible = !lines.empty();
                uiResult->values = std::move(lines);
            }

            // Two-tier verdict
            bool hasVisual = (overhangs > 0) || (thinSections > 0);
            bool hasPrecision = (smallFeatures > 0) || (sharpEdges > 0);

            glm::vec4 passColor = glm::vec4(0.4f, 0.8f, 0.4f, 1.0f);
            glm::vec4 failColor = glm::vec4(0.9f, 0.4f, 0.4f, 1.0f);

            std::vector<SectionLine> verdictLines;
            if (hasVisual && hasPrecision)
                verdictLines.push_back({"Some areas might not print well or accurately", "", failColor});
            else if (hasVisual)
                verdictLines.push_back({"Some areas might not print well", "", failColor});
            else if (hasPrecision)
                verdictLines.push_back({"Some areas might not print accurately", "", failColor});
            else
            {
                verdictLines.push_back({"No issues detected", "", passColor});

                // Contextual printing tip based on model geometry
                glm::dvec3 modelMin(std::numeric_limits<double>::max());
                glm::dvec3 modelMax(std::numeric_limits<double>::lowest());
                size_t totalLoops = 0;

                for (const auto &face : scene->faces)
                {
                    totalLoops += face.loops.size();
                    for (const auto &loop : face.loops)
                        for (const auto &oe : loop)
                        {
                            const auto &p = oe.edge->startPoint->position;
                            modelMin = glm::min(modelMin, p);
                            modelMax = glm::max(modelMax, p);
                        }
                }

                double height = modelMax.z - modelMin.z;
                double footprintX = modelMax.x - modelMin.x;
                double footprintY = modelMax.y - modelMin.y;
                double footprintArea = footprintX * footprintY;
                double footprintDiag = std::sqrt(footprintX * footprintX + footprintY * footprintY);

                struct Tip
                {
                    const char *text;
                    float weight;
                };
                std::vector<Tip> tips = {
                    {"Remember to clean your build plate!", 1.0f},
                    {"A brim can help with bed adhesion", 1.0f},
                    {"Keep your filament dry", 1.0f},
                    {"A retraction test can help reduce stringing", 0.5f},
                    {"Level your bed before printing", 1.0f},
                    {"Check your nozzle for wear", 0.5f},
                };

                // Tall & narrow → adhesion tips
                if (footprintDiag > 0 && height / footprintDiag > 1.5)
                {
                    tips[0].weight += 3.0f; // clean build plate
                    tips[1].weight += 3.0f; // brim
                }

                // Large footprint → level bed matters more
                if (footprintArea > 2500.0) // > ~50x50 mm
                    tips[4].weight += 3.0f;

                // Many loops → complex geometry → stringing risk
                if (totalLoops > 30)
                {
                    tips[2].weight += 2.0f; // dry filament
                    tips[3].weight += 2.0f; // retraction test
                }
                if (totalLoops > 80)
                {
                    tips[2].weight += 2.0f;
                    tips[3].weight += 2.0f;
                }

                // Weighted random pick
                float totalWeight = 0;
                for (const auto &t : tips)
                    totalWeight += t.weight;

                static std::mt19937 rng(std::random_device{}());
                std::uniform_real_distribution<float> dist(0.0f, totalWeight);
                float r = dist(rng);
                const char *tip = tips[0].text;
                float cumulative = 0;
                for (const auto &t : tips)
                {
                    cumulative += t.weight;
                    if (r < cumulative)
                    {
                        tip = t.text;
                        break;
                    }
                }

                glm::vec4 tipColor(0.55f, 0.55f, 0.55f, 1.0f);
                verdictLines.push_back({tip, "", tipColor});
            }

            if (uiVerdict)
                uiVerdict->values = std::move(verdictLines);
        }
        else
        {
            analysisRenderer.Clear();
            if (uiResult)
                uiResult->values = {};
            if (uiVerdict)
                uiVerdict->values = {};
        }
        sceneDirty = false;
    }

    if (renderDirty)
    {
        Render();
        renderDirty = false;
    }
}

void Display::SetAspectRatio(const uint16_t width, const uint16_t height)
{
    glViewport(0, 0, width, height);
    camera.SetAspectRatio(static_cast<float>(width) / static_cast<float>(height));
    uiRenderer.SetScreenSize(width, height);

    UpdateCamera();
    renderDirty = true;
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
    RootPanel filesDef;
    filesDef.id = "Files";
    filesDef.color = Color::GetUI(1);
    filesDef.leftAnchor = PanelAnchor{nullptr, PanelAnchor::Left};
    filesDef.rightAnchor = PanelAnchor{nullptr, PanelAnchor::Right};
    filesDef.topAnchor = PanelAnchor{nullptr, PanelAnchor::Top};
    filesDef.minWidth = sidebarWidth;
    filesDef.showLabel = false;
    RootPanel &files = uiRenderer.AddPanel(filesDef);
    files.children.reserve(1);
    files.AddParagraph("Files").values.emplace_back().text = "Files";

    // Analysis panel with sections
    RootPanel analysisDef;
    analysisDef.id = "Analysis";
    analysisDef.color = Color::GetUI(1);
    analysisDef.leftAnchor = PanelAnchor{nullptr, PanelAnchor::Left};
    analysisDef.topAnchor = PanelAnchor{&files, PanelAnchor::Bottom};
    analysisDef.showLabel = false;
    RootPanel &analysis = uiRenderer.AddPanel(analysisDef);

#if 1                             // DEBUG: panel-only mode — sections/content hidden for layout debugging
    analysis.children.reserve(5); // stable pointers: title + Result + ImportAction + Verdict + Configs
    analysis.AddParagraph("Analysis").values.emplace_back().text = "Analysis";
    uiResult = &analysis.AddParagraph("Result");
    uiResult->visible = false;
    uiImportPara = &analysis.AddParagraph("ImportAction");
    Paragraph &importPara = *uiImportPara;
    SectionLine &importLine = importPara.values.emplace_back();
    importLine.text = "Import file";
    importLine.onClick = [this]()
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
    };
    uiVerdict = &analysis.AddParagraph("Verdict");
    uiVerdict->visible = false;

    // Parameter group
    uiConfig = &analysis.AddSection("Configs");
    Section &config = *uiConfig;

    config.visible = false;
    Paragraph &configParams = config.AddParagraph("ConfigParams");
    configParams.values.reserve(4);
    // layer=2 matches the nesting depth of paragraphs inside sections, used for input background color
    constexpr int kInputLayer = 2;
    SectionLine &overhangContent = configParams.values.emplace_back();
    overhangContent.imguiContent = [this, layer = kInputLayer](float w, float h)
    {
        static bool requestEdit = false, editing = false, tracking = false, focusPending = false;
        static ImVec2 startPos;
        glm::vec4 c = glm::vec4(Color::GetFace(FaceFlawKind::OVERHANG).r + 0.4f,
                                Color::GetFace(FaceFlawKind::OVERHANG).g + 0.2f,
                                Color::GetFace(FaceFlawKind::OVERHANG).b + 0.2f, 1.0f);
        glm::vec4 tc = Color::GetUIText(1);
        glm::vec4 bg = Color::GetInputBg(layer);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, h * 0.3f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(tc.r, tc.g, tc.b, tc.a));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(bg.r, bg.g, bg.b, bg.a));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.15f, 0.15f, 0.15f, 0.5f));
        float normalPad = ImGui::GetStyle().FramePadding.x;
        float labelW = ImGui::CalcTextSize("Overhang:  ").x;
        float unitW = ImGui::CalcTextSize("\u00b0").x + normalPad * 2;
        float originX = ImGui::GetCursorScreenPos().x;
        if (requestEdit)
        {
            ImGui::SetKeyboardFocusHere();
            requestEdit = false;
            focusPending = true;
        }
        bool showEdit = editing || focusPending;
        if (showEdit)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelW);
            ImGui::SetNextItemWidth(w - labelW - unitW);
        }
        else
            ImGui::SetNextItemWidth(w);
        bool changed = ImGui::DragFloat("##overhang", &overhangAngle, 0.5f, 0.0f, 90.0f,
                                        showEdit ? "%.0f" : "");
        editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
        if (editing)
            focusPending = false;
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
                requestEdit = true;
            tracking = false;
        }
        float ty = ImGui::GetItemRectMin().y + ImGui::GetStyle().FramePadding.y;
        ImU32 col = ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, c.a));
        ImU32 valCol = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
        ImGui::GetWindowDrawList()->AddText(ImVec2(originX + normalPad, ty), col, "Overhang:");
        if (!showEdit)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0f\u00b0", overhangAngle);
            ImVec2 vs = ImGui::CalcTextSize(buf);
            ImGui::GetWindowDrawList()->AddText(ImVec2(originX + w - normalPad - vs.x, ty), valCol, buf);
        }
        else
        {
            const char *unit = "\u00b0";
            ImVec2 us = ImGui::CalcTextSize(unit);
            ImGui::GetWindowDrawList()->AddText(ImVec2(originX + w - normalPad - us.x, ty), valCol, unit);
        }
        if (changed)
        {
            RebuildAnalysis();
            UpdateScene();
        }
        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(4);
    };

    SectionLine &sharpContent = configParams.values.emplace_back();
    sharpContent.imguiContent = [this, layer = kInputLayer](float w, float h)
    {
        static bool requestEdit = false, editing = false, tracking = false, focusPending = false;
        static ImVec2 startPos;
        glm::vec4 c = glm::vec4(Color::GetEdge(EdgeFlawKind::SHARP_CORNER).r + 0.3f,
                                Color::GetEdge(EdgeFlawKind::SHARP_CORNER).g + 0.1f,
                                Color::GetEdge(EdgeFlawKind::SHARP_CORNER).b + 0.1f, 1.0f);
        glm::vec4 tc = Color::GetUIText(1);
        glm::vec4 bg = Color::GetInputBg(layer);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, h * 0.3f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(tc.r, tc.g, tc.b, tc.a));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(bg.r, bg.g, bg.b, bg.a));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.15f, 0.15f, 0.15f, 0.5f));
        float normalPad = ImGui::GetStyle().FramePadding.x;
        float labelW = ImGui::CalcTextSize("Sharp corner:  ").x;
        float unitW = ImGui::CalcTextSize("\u00b0").x + normalPad * 2;
        float originX = ImGui::GetCursorScreenPos().x;
        if (requestEdit)
        {
            ImGui::SetKeyboardFocusHere();
            requestEdit = false;
            focusPending = true;
        }
        bool showEdit = editing || focusPending;
        if (showEdit)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelW);
            ImGui::SetNextItemWidth(w - labelW - unitW);
        }
        else
            ImGui::SetNextItemWidth(w);
        bool changed = ImGui::DragFloat("##sharp", &sharpCornerAngle, 0.5f, 0.0f, 180.0f,
                                        showEdit ? "%.0f" : "");
        editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
        if (editing)
            focusPending = false;
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
                requestEdit = true;
            tracking = false;
        }
        float ty = ImGui::GetItemRectMin().y + ImGui::GetStyle().FramePadding.y;
        ImU32 col = ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, c.a));
        ImU32 valCol = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
        ImGui::GetWindowDrawList()->AddText(ImVec2(originX + normalPad, ty), col, "Sharp corner:");
        if (!showEdit)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0f\u00b0", sharpCornerAngle);
            ImVec2 vs = ImGui::CalcTextSize(buf);
            ImGui::GetWindowDrawList()->AddText(ImVec2(originX + w - normalPad - vs.x, ty), valCol, buf);
        }
        else
        {
            const char *unit = "\u00b0";
            ImVec2 us = ImGui::CalcTextSize(unit);
            ImGui::GetWindowDrawList()->AddText(ImVec2(originX + w - normalPad - us.x, ty), valCol, unit);
        }
        if (changed)
        {
            RebuildAnalysis();
            UpdateScene();
        }
        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(4);
    };

    SectionLine &featureContent = configParams.values.emplace_back();
    featureContent.imguiContent = [this, layer = kInputLayer](float w, float h)
    {
        static bool requestEdit = false, editing = false, tracking = false, focusPending = false;
        static ImVec2 startPos;
        glm::vec4 c = glm::vec4(Color::GetFace(FaceFlawKind::THIN_SECTION).r + 0.4f,
                                Color::GetFace(FaceFlawKind::THIN_SECTION).g + 0.4f,
                                Color::GetFace(FaceFlawKind::THIN_SECTION).b + 0.2f, 1.0f);
        glm::vec4 tc = Color::GetUIText(1);
        glm::vec4 bg = Color::GetInputBg(layer);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, h * 0.3f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(tc.r, tc.g, tc.b, tc.a));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(bg.r, bg.g, bg.b, bg.a));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.15f, 0.15f, 0.15f, 0.5f));
        float normalPad = ImGui::GetStyle().FramePadding.x;
        float labelW = ImGui::CalcTextSize("Min feature:  ").x;
        float unitW = ImGui::CalcTextSize("mm").x + normalPad * 2;
        float originX = ImGui::GetCursorScreenPos().x;
        if (requestEdit)
        {
            ImGui::SetKeyboardFocusHere();
            requestEdit = false;
            focusPending = true;
        }
        bool showEdit = editing || focusPending;
        if (showEdit)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelW);
            ImGui::SetNextItemWidth(w - labelW - unitW);
        }
        else
            ImGui::SetNextItemWidth(w);
        bool changed = ImGui::DragFloat("##feature", &minFeatureSize, 0.05f, 0.1f, 50.0f,
                                        showEdit ? "%.1f" : "");
        editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
        if (editing)
            focusPending = false;
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
                requestEdit = true;
            tracking = false;
        }
        float ty = ImGui::GetItemRectMin().y + ImGui::GetStyle().FramePadding.y;
        ImU32 col = ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, c.a));
        ImU32 valCol = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
        ImGui::GetWindowDrawList()->AddText(ImVec2(originX + normalPad, ty), col, "Min feature:");
        if (!showEdit)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f mm", minFeatureSize);
            ImVec2 vs = ImGui::CalcTextSize(buf);
            ImGui::GetWindowDrawList()->AddText(ImVec2(originX + w - normalPad - vs.x, ty), valCol, buf);
        }
        else
        {
            const char *unit = "mm";
            ImVec2 us = ImGui::CalcTextSize(unit);
            ImGui::GetWindowDrawList()->AddText(ImVec2(originX + w - normalPad - us.x, ty), valCol, unit);
        }
        if (changed)
        {
            RebuildAnalysis();
            UpdateScene();
        }
        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(4);
    };

    SectionLine &layerContent = configParams.values.emplace_back();
    layerContent.imguiContent = [this, layer = kInputLayer](float w, float h)
    {
        static bool requestEdit = false, editing = false, tracking = false, focusPending = false;
        static ImVec2 startPos;
        glm::vec4 c = Color::GetUIText(1);
        glm::vec4 bg = Color::GetInputBg(layer);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, h * 0.3f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c.r, c.g, c.b, c.a));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(bg.r, bg.g, bg.b, bg.a));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.15f, 0.15f, 0.15f, 0.5f));
        float normalPad = ImGui::GetStyle().FramePadding.x;
        float labelW = ImGui::CalcTextSize("Layer height:  ").x;
        float unitW = ImGui::CalcTextSize("mm").x + normalPad * 2;
        float originX = ImGui::GetCursorScreenPos().x;
        if (requestEdit)
        {
            ImGui::SetKeyboardFocusHere();
            requestEdit = false;
            focusPending = true;
        }
        bool showEdit = editing || focusPending;
        if (showEdit)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelW);
            ImGui::SetNextItemWidth(w - labelW - unitW);
        }
        else
            ImGui::SetNextItemWidth(w);
        bool changed = ImGui::DragFloat("##layer", &layerHeight, 0.01f, 0.01f, 5.0f,
                                        showEdit ? "%.2f" : "");
        editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
        if (editing)
            focusPending = false;
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
                requestEdit = true;
            tracking = false;
        }
        float ty = ImGui::GetItemRectMin().y + ImGui::GetStyle().FramePadding.y;
        ImU32 col = ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, c.a));
        ImGui::GetWindowDrawList()->AddText(ImVec2(originX + normalPad, ty), col, "Layer height:");
        if (!showEdit)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.2f mm", layerHeight);
            ImVec2 vs = ImGui::CalcTextSize(buf);
            ImGui::GetWindowDrawList()->AddText(ImVec2(originX + w - normalPad - vs.x, ty), col, buf);
        }
        else
        {
            const char *unit = "mm";
            ImVec2 us = ImGui::CalcTextSize(unit);
            ImGui::GetWindowDrawList()->AddText(ImVec2(originX + w - normalPad - us.x, ty), col, unit);
        }
        if (changed)
        {
            RebuildAnalysis();
            UpdateScene();
        }
        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(4);
    };

    RebuildAnalysis();
#endif // DEBUG: panel-only mode

    // Compute minimum grid extent and enforce as SDL minimum window size
    uiRenderer.ComputeMinGridSize();
    const auto &grid = uiRenderer.GetGrid();
    SDL_SetWindowMinimumSize(window, grid.MinWidthPixels(), grid.MinHeightPixels());
}