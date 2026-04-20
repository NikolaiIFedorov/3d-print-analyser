#include "display.hpp"
#include "rendering/color.hpp"
#include "rendering/UIRenderer/UIStyle.hpp"
#include "rendering/UIRenderer/Icons.hpp"
#include "logic/Analysis/Analysis.hpp"
#include "logic/Analysis/Overhang/Overhang.hpp"
#include "logic/Analysis/SharpCorner/SharpCorner.hpp"
#include "logic/Analysis/SmallFeature/SmallFeature.hpp"
#include "logic/Analysis/ThinSection/ThinSection.hpp"
#include "logic/Import/STLImport.hpp"
#include "utils/SystemAccent.hpp"
#include "utils/SystemAppearance.hpp"

#include <unordered_set>
#include <queue>
#include <cstdio>
#include <random>
#include "logic/Import/OBJImport.hpp"
#include "logic/Import/ThreeMFImport.hpp"
#include "input/FileImport.hpp"
#include "utils/SessionLogger.hpp"

Display::Display(int16_t width, int16_t height, const char *title, Scene *scene) : window(InitWindow(width, height, title)), renderer(GetWindow()), viewportRenderer(GetWindow()), uiRenderer(GetWindow(), "/System/Library/Fonts/SFNS.ttf"), camera(width, height), scene(scene)
{
    // Apply system appearance and accent color before any UI is constructed
    {
        Color::SetAppearance(SystemAppearance::IsDark());
        viewportRenderer.RegenerateGrid(); // grid was generated before SetAppearance; rebuild with correct mode
        float hue, sat;
        if (SystemAccent::GetHueSat(hue, sat))
            Color::SetAccent(hue, sat);
    }

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    Color::IsDark() ? ImGui::StyleColorsDark() : ImGui::StyleColorsLight();
    ImFontConfig avenirCfg;
    avenirCfg.FontNo = 4; // Avenir Heavy — used for headers (textDepth >= 3) and as ImGui default
    ImFont *heavyFont = io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Avenir.ttc", 19.0f, &avenirCfg);
    ImFontConfig avenirBookCfg;
    avenirBookCfg.FontNo = 0; // Avenir Book — used for body text (textDepth <= 2)
    ImFont *bodyFont = io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Avenir.ttc", 17.0f, &avenirBookCfg);
    ImFont *pixelFont = io.Fonts->AddFontDefault();
    ImGui_ImplSDL3_InitForOpenGL(window, glContext);
    ImGui_ImplOpenGL3_Init("#version 330");
    uiRenderer.SetPixelImFont(pixelFont);
    uiRenderer.SetBodyImFont(bodyFont);
    uiRenderer.SetHeavyImFont(heavyFont);

    InitUI();
    SDL_AddEventWatch(ResizeEventWatcher, this);

    // Live appearance change: update colors and redraw
    SystemAppearance::SetChangeCallback([this]()
                                        {
        bool dark = SystemAppearance::IsDark();
        Color::SetAppearance(dark);
        dark ? ImGui::StyleColorsDark() : ImGui::StyleColorsLight();
        uiRenderer.MarkDirty();
        viewportRenderer.RegenerateGrid(); // grid/axis colors are baked — regenerate
        // Geometry colors are baked into vertex buffers — rebuild if a model is loaded
        if (!this->scene->solids.empty() || !this->scene->faces.empty())
            UpdateScene();
        renderDirty = true; });

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
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

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
    SystemAppearance::ClearChangeCallback();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_RemoveEventWatch(ResizeEventWatcher, this);
    uiRenderer.Shutdown();
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
    Analysis::Instance().AddSolidAnalysis(std::make_unique<SmallFeature>(layerHeight, minFeatureSize));
    Analysis::Instance().AddSolidAnalysis(std::make_unique<ThinSection>(layerHeight, thinMinWidth));
    Analysis::Instance().AddEdgeAnalysis(std::make_unique<SharpCorner>(sharpCornerAngle));
}

void Display::UpdateCamera()
{
    cameraDirty = true;
    renderDirty = true;
}

void Display::Render()
{
    auto bg = Color::GetBase();
    glClearColor(bg.r, bg.g, bg.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    viewportRenderer.Render(); // grid (depth-tested)

    // Mark only the solid surface pixels in the stencil buffer (value = 1).
    // Lines are excluded — their geometry-shader quads extend beyond silhouettes
    // and would bleed into the stencil, incorrectly clipping axes.
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    renderer.RenderPatches();
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP); // stop writing before lines
    renderer.RenderWireframe();
    glDisable(GL_STENCIL_TEST);

    viewportRenderer.RenderAxes(); // axes — occluded by solid via stencil

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
        uiRenderer.MarkDirty();

        AnalysisResults results;
        if (analysisEnabled)
            results = Analysis::Instance().AnalyzeScene(scene);

        renderer.UpdateScene(scene, analysisEnabled ? &results : nullptr);
        if (analysisEnabled)
        {
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

            // Collect thin-section and small-feature faces for BFS grouping
            std::unordered_set<const Face *> thinSectionFaces;
            std::unordered_set<const Face *> smallFeatureFaces;
            for (const auto &[solid, flaws] : results.faceFlawRanges)
            {
                for (const auto &ff : flaws)
                {
                    switch (ff.flaw)
                    {
                    case FaceFlawKind::THIN_SECTION:
                        if (ff.face && !ff.face->loops.empty())
                            thinSectionFaces.insert(ff.face);
                        break;
                    case FaceFlawKind::SMALL_FEATURE:
                        if (ff.face && !ff.face->loops.empty())
                            smallFeatureFaces.insert(ff.face);
                        break;
                    default:
                        break;
                    }
                }
            }

            // Count connected components of adjacent thin-section faces
            {
                std::unordered_set<const Face *> visited;
                for (const Face *seed : thinSectionFaces)
                {
                    if (visited.count(seed))
                        continue;
                    thinSections++;
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
                                    if (thinSectionFaces.count(neighbor) && !visited.count(neighbor))
                                    {
                                        visited.insert(neighbor);
                                        bfs.push(neighbor);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Count connected components of adjacent small-feature faces
            {
                std::unordered_set<const Face *> visited;
                for (const Face *seed : smallFeatureFaces)
                {
                    if (visited.count(seed))
                        continue;
                    smallFeatures++;
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
                                    if (smallFeatureFaces.count(neighbor) && !visited.count(neighbor))
                                    {
                                        visited.insert(neighbor);
                                        bfs.push(neighbor);
                                    }
                                }
                            }
                        }
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
                                            Color::GetFace(FaceFlawKind::THIN_SECTION).g + 0.3f,
                                            Color::GetFace(FaceFlawKind::THIN_SECTION).b + 0.15f, 1.0f);
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

            // Write live flaw state — read each frame by the imguiContent lambdas in uiResult
            flawOverhang.count = overhangs;
            flawOverhang.frameCallback = makeFrameCallback(overhangMin, overhangMax);
            flawSharp.count = sharpEdges;
            flawSharp.frameCallback = makeFrameCallback(sharpMin, sharpMax);
            flawThin.count = thinSections;
            flawThin.frameCallback = makeFrameCallback(thinMin, thinMax);
            flawSmall.count = smallFeatures;
            flawSmall.frameCallback = makeFrameCallback(smallMin, smallMax);

            // Log analysis results to session
            {
                auto &sl = SessionLogger::Instance();
                sl.state.overhangs = overhangs;
                sl.state.sharpEdges = sharpEdges;
                sl.state.thinSections = thinSections;
                sl.state.smallFeatures = smallFeatures;
                sl.LogAnalysisRun();
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
                    tips[3].weight += 3.0f;

                // Only re-roll the tip when transitioning from flawed → pass
                if (!lastVerdictWasPass)
                {
                    float totalWeight = 0;
                    for (const auto &t : tips)
                        totalWeight += t.weight;

                    static std::mt19937 rng(std::random_device{}());
                    std::uniform_real_distribution<float> dist(0.0f, totalWeight);
                    float r = dist(rng);
                    cachedTip = tips[0].text;
                    float cumulative = 0;
                    for (const auto &t : tips)
                    {
                        cumulative += t.weight;
                        if (r < cumulative)
                        {
                            cachedTip = t.text;
                            break;
                        }
                    }
                }

                glm::vec4 tipColor(0.55f, 0.55f, 0.55f, 1.0f);
                verdictLines.push_back({cachedTip, "", tipColor});
            }

            bool verdictIsPass = !hasVisual && !hasPrecision;
            lastVerdictWasPass = verdictIsPass;
            if (uiVerdict)
                uiVerdict->values = std::move(verdictLines);
        }
        else
        {
            lastVerdictWasPass = false;
            flawOverhang = {};
            flawSharp = {};
            flawThin = {};
            flawSmall = {};
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
    windowWidth = static_cast<int16_t>(width);
    windowHeight = static_cast<int16_t>(height);

    // Use physical pixels for the GL viewport so Retina/HiDPI framebuffers
    // are covered correctly. Logical dimensions are still used for the camera
    // and UI (aspect ratio is identical; UI uses its own coordinate space).
    int physW, physH;
    SDL_GetWindowSizeInPixels(window, &physW, &physH);
    glViewport(0, 0, physW, physH);

    camera.SetAspectRatio(static_cast<float>(width) / static_cast<float>(height));
    uiRenderer.SetScreenSize(width, height);

    // Push updated matrices to the renderers immediately. ResizeEventWatcher
    // calls Render() before Frame() has a chance to process cameraDirty, so
    // the renderer must have the fresh projection before that Render() runs.
    renderer.SetCamera(camera);
    viewportRenderer.SetCamera(camera);

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

void Display::Orbit(float offsetX, float offsetY)
{
    camera.Orbit(offsetX, offsetY);

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

void Display::MarkBug()
{
    auto &sl = SessionLogger::Instance();
    sl.state.cameraTarget = camera.target;
    sl.state.cameraOrthoSize = camera.orthoSize;
    sl.LogBugMarker();
}

void Display::InitUI()
{
    float sidebarWidth = 10.0f;

    // Header
    RootPanel filesDef;
    filesDef.id = "Files";
    filesDef.colorDepth = 1;
    filesDef.leftAnchor = PanelAnchor{nullptr, PanelAnchor::Left};
    filesDef.rightAnchor = PanelAnchor{nullptr, PanelAnchor::Right};
    filesDef.topAnchor = PanelAnchor{nullptr, PanelAnchor::Top};
    filesDef.minWidth = sidebarWidth;
    RootPanel &files = uiRenderer.AddPanel(filesDef);
    files.children.reserve(1);
    files.AddParagraph("Files").values.emplace_back().text = "Files";

    // Analysis panel with sections
    RootPanel analysisDef;
    analysisDef.id = "Analysis";
    analysisDef.colorDepth = 1;
    analysisDef.leftAnchor = PanelAnchor{nullptr, PanelAnchor::Left};
    analysisDef.topAnchor = PanelAnchor{&files, PanelAnchor::Bottom};
    RootPanel &analysis = uiRenderer.AddPanel(analysisDef);

#if 1 // DEBUG: panel-only mode — sections/content hidden for layout debugging
    analysis.header = Header{"Analysis", 1.0f, 2};
    analysis.children.reserve(4); // stable pointers: Result + ImportAction + Verdict + Configs
    uiResult = &analysis.AddParagraph("Result");
    uiResult->visible = false;
    uiImportPara = &analysis.AddParagraph("ImportAction");
    Paragraph &importPara = *uiImportPara;
    SectionLine &importLine = importPara.values.emplace_back();
    importLine.text = "Import file";
    importLine.iconDraw = Icons::ImportFile();
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
                UpdateScene();

                // Log the import event to the session
                {
                    auto &sl = SessionLogger::Instance();
                    sl.state.lastFilename = path.substr(path.find_last_of("/\\") + 1);
                    sl.state.lastFormat = lower;
                    sl.state.points = this->scene->points.size();
                    sl.state.edges  = this->scene->edges.size();
                    sl.state.faces  = this->scene->faces.size();
                    sl.state.solids = this->scene->solids.size();
                    sl.LogFileImport(sl.state.lastFilename, lower);
                } });
    };
    uiVerdict = &analysis.AddParagraph("Verdict");
    uiVerdict->visible = false;

    // Merged result+config rows — always present once a model is loaded.
    // Each row shows: [icon] [count label (flaw color)] · [param value (dim)] [unit]
    // DragFloat spans the full row; a left-zone InvisibleButton handles click-to-navigate.
    uiResult = &analysis.AddParagraph("Result");
    uiResult->visible = false;
    uiResult->values.reserve(4);

    // Helper: builds an imguiContent lambda for a merged flaw+param row.
    // flawResult    = member to read count/frameCallback from (captured by ref via this)
    // flawColor     = bright flaw color for count+label text
    // countLabel    = e.g. " overhang"  (leading space, singular; "s" appended when count>1)
    // paramLabel    = short label shown dim to the right of the value, e.g. "°" or " mm"
    // getValue      = getter lambda returning float
    // setValue      = setter lambda (float)
    // dragSpeed/min/max/fmt = DragFloat parameters

    auto makeFlawRow = [this](
                           SectionLine &line,
                           glm::vec4 flawColor,
                           Icons::DrawFn icon,
                           const char *countLabel, // singular, e.g. " overhang"
                           const char *plural,     // suffix when count>1, e.g. "s"
                           FlawResult Display::*flawMember,
                           float &param,
                           float dragSpeed, float dragMin, float dragMax,
                           const char *unit, // e.g. "°" or "mm"
                           const char *dragId,
                           const char *minWidthLabel, // e.g. "45°" for min width estimate
                           bool isAngle               // true = format "%.0f", false = "%.2f"/"%.1f" based on dragSpeed
                       )
    {
        line.iconDraw = [this, flawMember, icon](ImDrawList *dl, float x, float midY, float s)
        {
            FlawResult &fr = this->*flawMember;
            if (fr.count == 0)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.35f);
            icon(dl, x, midY, s);
            if (fr.count == 0)
                ImGui::PopStyleVar();
        };

        line.imguiContent = [this, flawColor, countLabel, plural, flawMember,
                             &param, dragSpeed, dragMin, dragMax,
                             unit, dragId, isAngle](float w, float h, float iconOffset)
        {
            FlawResult &fr = this->*flawMember;
            glm::vec4 dimColor = Color::GetUIText(1);
            glm::vec4 dimLow = Color::GetUIText(0);

            UIStyle::PushInputStyle(h, dimColor);
            float normalPad = ImGui::GetStyle().FramePadding.x;
            ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
            float originX = rowOrigin.x;

            // ── Left nav zone: InvisibleButton placed BEFORE DragFloat ──────────
            // Compute left zone width from count label (CalcTextSize valid here — inside a frame)
            char countBuf[64];
            snprintf(countBuf, sizeof(countBuf), "%zu%s%s", fr.count, countLabel,
                     fr.count > 1 ? plural : "");
            float leftW = iconOffset + ImGui::CalcTextSize(countBuf).x + normalPad * 2.5f;
            leftW = std::min(leftW, w * 0.65f); // never crowd out the param zone

            bool navFired = false;
            bool showEdit = fr.editing || fr.focusPending;

            if (!showEdit)
            {
                char navId[64];
                snprintf(navId, sizeof(navId), "##nav%s", dragId);
                ImGui::InvisibleButton(navId, ImVec2(leftW, h));

                if (ImGui::IsItemActivated())
                {
                    fr.navTracking = true;
                    fr.navStart = ImGui::GetIO().MousePos;
                }
                if (fr.navTracking && !ImGui::IsItemActive())
                {
                    ImVec2 ep = ImGui::GetIO().MousePos;
                    float d = (ep.x - fr.navStart.x) * (ep.x - fr.navStart.x) +
                              (ep.y - fr.navStart.y) * (ep.y - fr.navStart.y);
                    if (d < 9.0f && fr.count > 0 && fr.frameCallback)
                        navFired = true;
                    fr.navTracking = false;
                }
                // Hover tint on left zone (only when there's something to navigate to)
                if (fr.count > 0 && fr.frameCallback)
                    UIStyle::DrawInputHoverTint(1);
                else if (ImGui::IsItemHovered())
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow); // no pointer when non-navigable
            }

            // Always position DragFloat at the right zone — even in edit mode.
            // Add a small gap in edit mode so the input field doesn't butt up against the label.
            float editGap = showEdit ? normalPad * 3.0f : 0.0f;
            ImGui::SetCursorScreenPos(ImVec2(originX + leftW + editGap, rowOrigin.y));

            // ── Right param zone: DragFloat ──────────────────────────────────────
            float rightW = w - leftW - editGap;

            if (fr.requestEdit)
            {
                ImGui::SetKeyboardFocusHere();
                fr.requestEdit = false;
                fr.focusPending = true;
                showEdit = true;
            }

            ImGui::SetNextItemWidth(rightW);
            const char *fmt = showEdit ? (isAngle ? "%.0f" : (dragSpeed < 0.1f ? "%.2f" : "%.1f")) : "";
            bool changed = ImGui::DragFloat(dragId, &param, dragSpeed, dragMin, dragMax, fmt);
            UIStyle::DrawInputHoverTint(1);

            fr.editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
            if (fr.editing)
                fr.focusPending = false;

            if (ImGui::IsItemActivated())
            {
                fr.tracking = true;
                fr.startPos = ImGui::GetIO().MousePos;
            }
            if (fr.tracking && !ImGui::IsItemActive())
            {
                ImVec2 ep = ImGui::GetIO().MousePos;
                float d = (ep.x - fr.startPos.x) * (ep.x - fr.startPos.x) +
                          (ep.y - fr.startPos.y) * (ep.y - fr.startPos.y);
                if (d < 9.0f)
                    fr.requestEdit = true;
                fr.tracking = false;
            }

            // ── Text overlay ─────────────────────────────────────────────────────
            float ty = ImGui::GetItemRectMin().y + ImGui::GetStyle().FramePadding.y;
            ImU32 flawCol = ImGui::GetColorU32(ImVec4(flawColor.r, flawColor.g, flawColor.b, flawColor.a));
            ImU32 dimCol = ImGui::GetColorU32(ImVec4(dimLow.r, dimLow.g, dimLow.b, dimLow.a));
            ImU32 dimColZero = ImGui::GetColorU32(ImVec4(dimLow.r, dimLow.g, dimLow.b, dimLow.a * 0.5f));

            // Left label — always visible (even during text edit)
            if (fr.count > 0)
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(originX + iconOffset + normalPad, ty), flawCol, countBuf);
            else
            {
                std::string noFlawLabel = std::string("No") + countLabel + plural;
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(originX + iconOffset + normalPad, ty), dimColZero, noFlawLabel.c_str());
            }

            // Right side: param value in normal mode, unit hint in edit mode
            if (!showEdit)
            {
                char valBuf[32];
                if (isAngle)
                    snprintf(valBuf, sizeof(valBuf), "%.0f%s", param, unit);
                else if (dragSpeed < 0.1f)
                    snprintf(valBuf, sizeof(valBuf), "%.2f %s", param, unit);
                else
                    snprintf(valBuf, sizeof(valBuf), "%.1f %s", param, unit);
                ImVec2 vs = ImGui::CalcTextSize(valBuf);
                ImU32 valCol = (fr.count > 0) ? dimCol : dimColZero;
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(originX + w - normalPad - vs.x, ty), valCol, valBuf);
            }
            else
            {
                ImVec2 us = ImGui::CalcTextSize(unit);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(originX + w - normalPad - us.x, ty), dimCol, unit);
            }

            if (navFired)
                fr.frameCallback();
            if (changed)
            {
                auto &sl = SessionLogger::Instance();
                sl.state.overhangAngle = this->overhangAngle;
                sl.state.sharpCornerAngle = this->sharpCornerAngle;
                sl.state.thinMinWidth = this->thinMinWidth;
                sl.state.minFeatureSize = this->minFeatureSize;
                sl.state.layerHeight = this->layerHeight;
                sl.LogParamChange(std::string(dragId + 2), param);
                RebuildAnalysis();
                UpdateScene();
            }
            UIStyle::PopInputStyle();
        };
    };

    glm::vec4 overhangColor = {Color::GetFace(FaceFlawKind::OVERHANG).r + 0.4f,
                               Color::GetFace(FaceFlawKind::OVERHANG).g + 0.2f,
                               Color::GetFace(FaceFlawKind::OVERHANG).b + 0.2f, 1.0f};
    glm::vec4 thinColor = {Color::GetFace(FaceFlawKind::THIN_SECTION).r + 0.4f,
                           Color::GetFace(FaceFlawKind::THIN_SECTION).g + 0.3f,
                           Color::GetFace(FaceFlawKind::THIN_SECTION).b + 0.15f, 1.0f};
    glm::vec4 smallColor = {Color::GetFace(FaceFlawKind::SMALL_FEATURE).r + 0.4f,
                            Color::GetFace(FaceFlawKind::SMALL_FEATURE).g + 0.4f,
                            Color::GetFace(FaceFlawKind::SMALL_FEATURE).b + 0.2f, 1.0f};
    glm::vec4 edgeColor = {Color::GetEdge(EdgeFlawKind::SHARP_CORNER).r + 0.3f,
                           Color::GetEdge(EdgeFlawKind::SHARP_CORNER).g + 0.1f,
                           Color::GetEdge(EdgeFlawKind::SHARP_CORNER).b + 0.1f, 1.0f};
    makeFlawRow(uiResult->values.emplace_back(), overhangColor,
                Icons::Overhang(overhangColor),
                " overhang", "s", &Display::flawOverhang,
                overhangAngle, 0.5f, 0.0f, 90.0f, "\u00b0", "##overhang", "90", true);

    makeFlawRow(uiResult->values.emplace_back(), edgeColor,
                Icons::SharpCorner(edgeColor),
                " sharp edge", "s", &Display::flawSharp,
                sharpCornerAngle, 0.5f, 0.0f, 180.0f, "\u00b0", "##sharp", "180", true);

    makeFlawRow(uiResult->values.emplace_back(), thinColor,
                Icons::ThinSection(thinColor),
                " thin section", "s", &Display::flawThin,
                thinMinWidth, 0.05f, 0.1f, 50.0f, "mm", "##thinsection", "2.0", false);

    makeFlawRow(uiResult->values.emplace_back(), smallColor,
                Icons::SmallFeature(smallColor),
                " small feature", "s", &Display::flawSmall,
                minFeatureSize, 0.05f, 0.1f, 50.0f, "mm", "##smallfeature", "10.0", false);

    RebuildAnalysis();

#endif // DEBUG: panel-only mode

    // Compute minimum grid extent and enforce as SDL minimum window size
    uiRenderer.ComputeMinGridSize();
    const auto &grid = uiRenderer.GetGrid();
    SDL_SetWindowMinimumSize(window, grid.MinWidthPixels(), grid.MinHeightPixels());
}