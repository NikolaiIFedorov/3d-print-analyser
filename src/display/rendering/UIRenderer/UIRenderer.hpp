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
    Panel *GetPanel(const std::string &id);
    void SetSectionValue(const std::string &panelId, const std::string &sectionId, const std::vector<SectionLine> &values);
    void SetSectionVisible(const std::string &panelId, const std::string &sectionId, bool visible);
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
    TextRenderer textRenderer;
    SDL_Window *window = nullptr;
    bool dirty = true;

    bool InitializeShaders();
    void ResolveAnchors();
    void BuildMesh();
};
