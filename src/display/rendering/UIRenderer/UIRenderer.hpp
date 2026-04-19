#pragma once

#include <glad/glad.h>
#include <SDL3/SDL.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <deque>
#include <string>
#include <vector>

#include "rendering/OpenGL/shaders/OpenGLShader.hpp"
#include "Panel.hpp"
#include "TextRenderer.hpp"
#include "UIGrid.hpp"

struct ImFont;

struct UIVertex
{
    glm::vec2 position;
    glm::vec4 color;
};

// Precomputed text metrics derived from grid + font, reused throughout layout and rendering.
struct TextMetrics
{
    float localCell;       // grid.cellSizeX * LOCAL_CELL_RATIO
    float textScale;       // localCell / lineHeight * 1.4
    float textHeightCells; // maxBearingY(textScale) / cellSizeY
};

class UIRenderer
{
public:
    UIRenderer() = default;
    UIRenderer(SDL_Window *window, const std::string &fontPath);
    ~UIRenderer();

    UIRenderer(const UIRenderer &) = delete;
    UIRenderer &operator=(const UIRenderer &) = delete;
    UIRenderer(UIRenderer &&other) noexcept;
    UIRenderer &operator=(UIRenderer &&other) noexcept;

    void SetScreenSize(int width, int height);
    void Render();
    bool HitTest(float pixelX, float pixelY) const;
    void Shutdown();

    RootPanel &AddPanel(const RootPanel &panel);
    RootPanel *GetPanel(const std::string &id);
    void SetSectionValue(const std::string &panelId, const std::string &sectionId, const std::vector<SectionLine> &values);
    void SetSectionVisible(const std::string &panelId, const std::string &sectionId, bool visible);
    void MarkDirty() { dirty = true; }
    const UIGrid &GetGrid() const { return grid; }
    void ComputeMinGridSize();
    void SetPixelImFont(ImFont *font) { pixelImFont = font; }
    ImFont *GetPixelImFont() const { return pixelImFont; }
    void SetBodyImFont(ImFont *font) { bodyImFont = font; }
    void SetHeavyImFont(ImFont *font) { heavyImFont = font; }
    void SetDebugLayout(bool v) { debugLayout = v; }
    bool GetDebugLayout() const { return debugLayout; }

private:
    OpenGLShader shader;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ibo = 0;
    uint32_t indexCount = 0;

    int screenWidth = 0;
    int screenHeight = 0;
    glm::mat4 projection = glm::mat4(1.0f);

    UIGrid grid;
    std::deque<RootPanel> panels;
    TextRenderer textRenderer;
    SDL_Window *window = nullptr;
    ImFont *pixelImFont = nullptr;
    ImFont *bodyImFont = nullptr;       // lighter weight font for body text (textDepth <= 2)
    ImFont *heavyImFont = nullptr;      // bold/header font; fallback for layout before first ImGui frame
    ImFont *cachedTextImFont = nullptr; // default ImGui font captured each frame in Render()
    bool dirty = true;
    bool debugLayout = false;

    bool InitializeShaders();
    void ResolveAnchors();
    void BuildMesh();
    TextMetrics ComputeTextMetrics() const;
    static void EmitRoundedRect(std::vector<UIVertex> &vertices, std::vector<uint32_t> &indices,
                                uint32_t &vertexOffset, float x0, float y0, float x1, float y1,
                                float radius, glm::vec4 color);
};
