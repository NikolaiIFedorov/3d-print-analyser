#pragma once

#include <glad/glad.h>
#include <SDL3/SDL.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <deque>
#include <vector>

#include "rendering/OpenGL/shaders/OpenGLShader.hpp"
#include "Button.hpp"
#include "Panel.hpp"
#include "TextRenderer.hpp"
#include "UIGrid.hpp"

struct UIVertex
{
    glm::vec2 position;
    glm::vec4 color;
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

    Panel &AddPanel(const Panel &panel);
    Panel &AddButton(const Panel &panel, std::function<void()> onClick);
    Panel *GetPanel(const std::string &id);
    void SetSectionValue(const std::string &panelId, const std::string &sectionId, const std::vector<SectionLine> &values);
    void SetSectionVisible(const std::string &panelId, const std::string &sectionId, bool visible);
    void SetSectionClick(const std::string &panelId, const std::string &sectionId, std::function<void()> onClick);
    void SetSectionSlider(const std::string &panelId, const std::string &sectionId,
                          double min, double max, double step, double *value,
                          std::function<void()> onChange);
    bool HandleClick(float pixelX, float pixelY);
    bool HandleMouseDown(float pixelX, float pixelY);
    bool HandleMouseMotion(float pixelX, float pixelY);
    void HandleMouseUp();
    const UIGrid &GetGrid() const { return grid; }
    void ComputeMinGridSize();

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
    std::deque<Panel> panels;
    std::vector<Button> buttons;
    std::vector<SectionButton> sectionButtons;
    std::vector<SectionSlider> sectionSliders;
    SectionSlider *activeSlider = nullptr;
    TextRenderer textRenderer;
    SDL_Window *window = nullptr;
    bool dirty = true;

    bool InitializeShaders();
    void ResolveAnchors();
    void BuildMesh();
};
