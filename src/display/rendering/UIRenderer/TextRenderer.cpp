#include "TextRenderer.hpp"
#include "utils/log.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <glm/gtc/matrix_transform.hpp>
#include <vector>

TextRenderer::~TextRenderer()
{
    Shutdown();
}

TextRenderer::TextRenderer(TextRenderer &&other) noexcept
    : shader(std::move(other.shader)),
      vao(other.vao), vbo(other.vbo), atlas(other.atlas),
      atlasWidth(other.atlasWidth), atlasHeight(other.atlasHeight),
      lineHeight(other.lineHeight),
      ascender(other.ascender),
      maxBearingY(other.maxBearingY),
      projection(other.projection),
      glyphs(std::move(other.glyphs))
{
    other.vao = other.vbo = other.atlas = 0;
}

TextRenderer &TextRenderer::operator=(TextRenderer &&other) noexcept
{
    if (this != &other)
    {
        Shutdown();
        shader = std::move(other.shader);
        vao = other.vao;
        vbo = other.vbo;
        atlas = other.atlas;
        atlasWidth = other.atlasWidth;
        atlasHeight = other.atlasHeight;
        lineHeight = other.lineHeight;
        ascender = other.ascender;
        maxBearingY = other.maxBearingY;
        projection = other.projection;
        glyphs = std::move(other.glyphs);
        other.vao = other.vbo = other.atlas = 0;
    }
    return *this;
}

bool TextRenderer::Init(const std::string &fontPath, uint32_t fontSize)
{
    if (!shader.LoadFromFiles("shaders/text.vert", "shaders/text.frag"))
        return LOG_FALSE("Failed to load text shaders");

    if (!BuildAtlas(fontPath, fontSize))
        return LOG_FALSE("Failed to build font atlas");

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    // 4 floats per vertex (x, y, u, v), 6 vertices per quad max — we'll stream
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * 6 * 128, nullptr, GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    LOG_VOID("Initialized TextRenderer");
    return true;
}

bool TextRenderer::BuildAtlas(const std::string &fontPath, uint32_t fontSize)
{
    FT_Library ft;
    if (FT_Init_FreeType(&ft))
        return LOG_FALSE("Failed to init FreeType");

    FT_Face face;
    if (FT_New_Face(ft, fontPath.c_str(), 0, &face))
    {
        FT_Done_FreeType(ft);
        return LOG_FALSE("Failed to load font: " + fontPath);
    }

    FT_Set_Pixel_Sizes(face, 0, fontSize);

    // First pass: measure total atlas dimensions
    int totalWidth = 0;
    int maxHeight = 0;

    for (unsigned char c = 32; c < 127; c++)
    {
        if (FT_Load_Char(face, c, FT_LOAD_RENDER))
            continue;

        totalWidth += static_cast<int>(face->glyph->bitmap.width) + 1; // +1 padding
        maxHeight = std::max(maxHeight, static_cast<int>(face->glyph->bitmap.rows));
    }

    atlasWidth = totalWidth;
    atlasHeight = maxHeight + 2; // +2 for 1px padding top and bottom
    lineHeight = static_cast<float>(face->size->metrics.height >> 6);
    ascender = static_cast<float>(face->size->metrics.ascender >> 6);

    // Create atlas texture
    glGenTextures(1, &atlas);
    glBindTexture(GL_TEXTURE_2D, atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED,
                 atlasWidth, atlasHeight, 0,
                 GL_RED, GL_UNSIGNED_BYTE, nullptr);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Second pass: blit glyphs into atlas (1px down from top edge)
    int xOffset = 0;
    float invW = 1.0f / static_cast<float>(atlasWidth);
    float invH = 1.0f / static_cast<float>(atlasHeight);

    for (unsigned char c = 32; c < 127; c++)
    {
        if (FT_Load_Char(face, c, FT_LOAD_RENDER))
            continue;

        auto &bmp = face->glyph->bitmap;

        glTexSubImage2D(GL_TEXTURE_2D, 0,
                        xOffset, 1,
                        static_cast<int>(bmp.width), static_cast<int>(bmp.rows),
                        GL_RED, GL_UNSIGNED_BYTE, bmp.buffer);

        GlyphInfo info;
        info.size = glm::ivec2(bmp.width, bmp.rows);
        info.bearing = glm::ivec2(face->glyph->bitmap_left, face->glyph->bitmap_top);
        info.advance = static_cast<uint32_t>(face->glyph->advance.x);
        info.uvX = static_cast<float>(xOffset) * invW;
        info.uvY = 1.0f * invH;
        info.uvW = static_cast<float>(bmp.width) * invW;
        info.uvH = static_cast<float>(bmp.rows) * invH;

        glyphs[static_cast<char>(c)] = info;
        maxBearingY = std::max(maxBearingY, static_cast<float>(info.bearing.y));

        xOffset += static_cast<int>(bmp.width) + 1;
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    FT_Done_Face(face);
    FT_Done_FreeType(ft);

    return true;
}

void TextRenderer::SetProjection(const glm::mat4 &proj)
{
    projection = proj;
}

void TextRenderer::RenderText(const std::string &text, float x, float y,
                              float scale, const glm::vec4 &color)
{
    if (text.empty() || atlas == 0)
        return;

    shader.Use();
    shader.SetMat4("uProjection", projection);
    shader.SetVec4("uTextColor", color);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas);

    glBindVertexArray(vao);

    // Build quads for all characters
    std::vector<float> vertices;
    vertices.reserve(text.size() * 6 * 4);

    float cursorX = x;
    for (char c : text)
    {
        auto it = glyphs.find(c);
        if (it == glyphs.end())
            continue;

        const auto &g = it->second;

        float xPos = cursorX + static_cast<float>(g.bearing.x) * scale;
        float yPos = y - static_cast<float>(g.bearing.y) * scale;
        float w = static_cast<float>(g.size.x) * scale;
        float h = static_cast<float>(g.size.y) * scale;

        float u0 = g.uvX;
        float v0 = g.uvY;
        float u1 = g.uvX + g.uvW;
        float v1 = g.uvY + g.uvH;

        // Two triangles per glyph quad
        // clang-format off
        float quad[] = {
            xPos,     yPos,     u0, v0,
            xPos + w, yPos,     u1, v0,
            xPos + w, yPos + h, u1, v1,

            xPos,     yPos,     u0, v0,
            xPos + w, yPos + h, u1, v1,
            xPos,     yPos + h, u0, v1,
        };
        // clang-format on

        vertices.insert(vertices.end(), std::begin(quad), std::end(quad));

        cursorX += static_cast<float>(g.advance >> 6) * scale;
    }

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    vertices.size() * sizeof(float), vertices.data());

    glDrawArrays(GL_TRIANGLES, 0, static_cast<int>(vertices.size()) / 4);

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

PixelBounds TextRenderer::MeasureBounds(const std::string &text, float x, float y, float scale) const
{
    PixelBounds bounds;
    float cursorX = x;
    for (char c : text)
    {
        auto it = glyphs.find(c);
        if (it == glyphs.end())
            continue;
        const auto &g = it->second;
        float xPos = cursorX + static_cast<float>(g.bearing.x) * scale;
        float yPos = y - static_cast<float>(g.bearing.y) * scale;
        float w = static_cast<float>(g.size.x) * scale;
        float h = static_cast<float>(g.size.y) * scale;
        // All four corners of the glyph quad — identical to RenderText's quad vertices
        bounds.expand(xPos, yPos);
        bounds.expand(xPos + w, yPos + h);
        cursorX += static_cast<float>(g.advance >> 6) * scale;
    }
    return bounds;
}

float TextRenderer::MeasureWidth(const std::string &text, float scale) const
{
    float width = 0.0f;
    for (char c : text)
    {
        auto it = glyphs.find(c);
        if (it == glyphs.end())
            continue;

        width += static_cast<float>(it->second.advance >> 6) * scale;
    }
    return width;
}

float TextRenderer::GetLineHeight(float scale) const
{
    return lineHeight * scale;
}

float TextRenderer::GetMaxBearingY(float scale) const
{
    return maxBearingY * scale;
}

void TextRenderer::Shutdown()
{
    if (atlas != 0)
    {
        glDeleteTextures(1, &atlas);
        atlas = 0;
    }
    if (vbo != 0)
    {
        glDeleteBuffers(1, &vbo);
        vbo = 0;
    }
    if (vao != 0)
    {
        glDeleteVertexArrays(1, &vao);
        vao = 0;
    }

    shader.Delete();
    glyphs.clear();
}
