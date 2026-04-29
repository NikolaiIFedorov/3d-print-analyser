#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>

#include <string>
#include <unordered_map>

#include "rendering/OpenGL/shaders/OpenGLShader.hpp"
#include "Panel.hpp"

struct GlyphInfo
{
    glm::ivec2 size;    // glyph pixel dimensions
    glm::ivec2 bearing; // offset from baseline to top-left
    uint32_t advance;   // horizontal advance (in 1/64 pixels)
    float uvX;          // x offset into atlas (normalized)
    float uvY;          // y offset into atlas (normalized)
    float uvW;          // width in atlas (normalized)
    float uvH;          // height in atlas (normalized)
};

class TextRenderer
{
public:
    TextRenderer() = default;
    ~TextRenderer();

    TextRenderer(const TextRenderer &) = delete;
    TextRenderer &operator=(const TextRenderer &) = delete;
    TextRenderer(TextRenderer &&other) noexcept;
    TextRenderer &operator=(TextRenderer &&other) noexcept;

    bool Init(const std::string &fontPath, uint32_t fontSize);
    void SetProjection(const glm::mat4 &proj);
    void RenderText(const std::string &text, float x, float y,
                    float scale, const glm::vec4 &color);
    void Shutdown();

    float MeasureWidth(const std::string &text, float scale) const;
    float GetLineHeight(float scale) const;
    float GetMaxBearingY(float scale) const;

    // Returns the pixel bounding box of the quads that RenderText would produce.
    // Derives bounds from the same glyph bearing/size/advance as the actual render path.
    PixelBounds MeasureBounds(const std::string &text, float x, float y, float scale) const;

private:
    OpenGLShader shader;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint atlas = 0;

    int atlasWidth = 0;
    int atlasHeight = 0;
    float lineHeight = 0.0f;
    float ascender = 0.0f;
    float maxBearingY = 0.0f;

    glm::mat4 projection = glm::mat4(1.0f);

    std::unordered_map<char, GlyphInfo> glyphs;

    bool BuildAtlas(const std::string &fontPath, uint32_t fontSize);
};
