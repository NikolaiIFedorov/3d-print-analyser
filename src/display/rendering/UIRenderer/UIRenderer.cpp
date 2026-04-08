#include "UIRenderer.hpp"
#include "utils/log.hpp"
#include <cmath>

UIRenderer::UIRenderer(SDL_Window *window)
{
    if (!InitializeShaders())
    {
        LOG_FALSE("Failed to initialize UI shaders");
        return;
    }

    glGenVertexArrays(1, &vao);

    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    SetScreenSize(w, h);

    LOG_VOID("Initialized UIRenderer");
}

UIRenderer::~UIRenderer()
{
    Shutdown();
}

UIRenderer::UIRenderer(UIRenderer &&other) noexcept
    : shader(std::move(other.shader)),
      vao(other.vao), vbo(other.vbo), ibo(other.ibo),
      indexCount(other.indexCount),
      screenWidth(other.screenWidth), screenHeight(other.screenHeight),
      projection(other.projection),
      grid(other.grid),
      panels(std::move(other.panels)),
      dirty(other.dirty)
{
    other.vao = other.vbo = other.ibo = 0;
    other.indexCount = 0;
}

UIRenderer &UIRenderer::operator=(UIRenderer &&other) noexcept
{
    if (this != &other)
    {
        Shutdown();
        shader = std::move(other.shader);
        vao = other.vao;
        vbo = other.vbo;
        ibo = other.ibo;
        indexCount = other.indexCount;
        screenWidth = other.screenWidth;
        screenHeight = other.screenHeight;
        projection = other.projection;
        grid = other.grid;
        panels = std::move(other.panels);
        dirty = other.dirty;
        other.vao = other.vbo = other.ibo = 0;
        other.indexCount = 0;
    }
    return *this;
}

bool UIRenderer::InitializeShaders()
{
    return shader.LoadFromFiles("shaders/ui.vert", "shaders/ui.frag");
}

void UIRenderer::SetScreenSize(int width, int height)
{
    screenWidth = width;
    screenHeight = height;
    projection = glm::ortho(0.0f, static_cast<float>(width),
                            static_cast<float>(height), 0.0f,
                            -1.0f, 1.0f);

    grid.Update(width, height);

    ResolveAnchors();

    dirty = true;
}

Panel &UIRenderer::AddPanel(const Panel &panel)
{
    panels.push_back(panel);
    dirty = true;
    return panels.back();
}

Panel *UIRenderer::GetPanel(const std::string &id)
{
    for (auto &panel : panels)
    {
        if (panel.id == id)
            return &panel;
    }
    return nullptr;
}

Panel &UIRenderer::AddButton(const Panel &panel, std::function<void()> onClick)
{
    panels.push_back(panel);
    buttons.push_back({panel.id, std::move(onClick)});
    dirty = true;
    return panels.back();
}

bool UIRenderer::HandleClick(float pixelX, float pixelY)
{
    if (grid.cellSizeX <= 0.0f || grid.cellSizeY <= 0.0f)
        return false;

    float cellX = pixelX / grid.cellSizeX;
    float cellY = pixelY / grid.cellSizeY;

    for (auto it = buttons.rbegin(); it != buttons.rend(); ++it)
    {
        const Panel *p = GetPanel(it->panelId);
        if (!p || !p->visible)
            continue;

        if (cellX >= p->col && cellX <= p->col + p->colSpan &&
            cellY >= p->row && cellY <= p->row + p->rowSpan)
        {
            if (it->onClick)
                it->onClick();
            return true;
        }
    }
    return false;
}

void UIRenderer::ResolveAnchors()
{
    // Resolve an anchor to a cell coordinate.
    // Gap is applied automatically: screen edges push inward, panel edges push outward.
    auto resolveEdge = [&](const PanelAnchor &anchor) -> float
    {
        const float gap = UIGrid::GAP;

        if (!anchor.panel)
        {
            // Screen-relative: inset by gap
            switch (anchor.edge)
            {
            case PanelAnchor::Right:
                return static_cast<float>(grid.columns) - gap;
            case PanelAnchor::Bottom:
                return static_cast<float>(grid.rows) - gap;
            default: // Left, Top
                return gap;
            }
        }

        const Panel &other = *anchor.panel;
        switch (anchor.edge)
        {
        case PanelAnchor::Left:
            return other.col - gap;
        case PanelAnchor::Right:
            return other.col + other.colSpan + gap;
        case PanelAnchor::Top:
            return other.row - gap;
        case PanelAnchor::Bottom:
            return other.row + other.rowSpan + gap;
        }
        return 0.0f;
    };

    // Single linear pass — panels can only reference panels added before them
    for (auto &panel : panels)
    {
        // --- Horizontal axis ---
        std::optional<float> left, right, w;
        if (panel.leftAnchor)
            left = resolveEdge(*panel.leftAnchor);
        if (panel.rightAnchor)
            right = resolveEdge(*panel.rightAnchor);
        w = panel.width;

        if (left && right)
        {
            panel.col = *left;
            panel.colSpan = *right - *left;
        }
        else if (left && w)
        {
            panel.col = *left;
            panel.colSpan = *w;
        }
        else if (right && w)
        {
            panel.col = *right - *w;
            panel.colSpan = *w;
        }

        // --- Vertical axis ---
        std::optional<float> top, bottom, h;
        if (panel.topAnchor)
            top = resolveEdge(*panel.topAnchor);
        if (panel.bottomAnchor)
            bottom = resolveEdge(*panel.bottomAnchor);
        h = panel.height;

        if (top && bottom)
        {
            panel.row = *top;
            panel.rowSpan = *bottom - *top;
        }
        else if (top && h)
        {
            panel.row = *top;
            panel.rowSpan = *h;
        }
        else if (bottom && h)
        {
            panel.row = *bottom - *h;
            panel.rowSpan = *h;
        }
    }
}

void UIRenderer::BuildMesh()
{
    ResolveAnchors();

    std::vector<UIVertex> vertices;
    std::vector<uint32_t> indices;

    uint32_t vertexOffset = 0;
    for (const auto &panel : panels)
    {
        if (!panel.visible)
            continue;

        float x0 = grid.ToPixelsX(panel.col);
        float y0 = grid.ToPixelsY(panel.row);
        float x1 = grid.ToPixelsX(panel.col + panel.colSpan);
        float y1 = grid.ToPixelsY(panel.row + panel.rowSpan);
        float r = std::min(grid.cellSizeX, grid.cellSizeY) * panel.borderRadius;

        // Clamp radius to half the smaller dimension
        float maxR = std::min((x1 - x0), (y1 - y0)) * 0.5f;
        if (r > maxR)
            r = maxR;

        if (r <= 0.0f)
        {
            // Simple quad (no rounding)
            vertices.push_back({{x0, y0}, panel.color});
            vertices.push_back({{x1, y0}, panel.color});
            vertices.push_back({{x1, y1}, panel.color});
            vertices.push_back({{x0, y1}, panel.color});

            indices.push_back(vertexOffset + 0);
            indices.push_back(vertexOffset + 1);
            indices.push_back(vertexOffset + 2);
            indices.push_back(vertexOffset + 0);
            indices.push_back(vertexOffset + 2);
            indices.push_back(vertexOffset + 3);

            vertexOffset += 4;
        }
        else
        {
            // Rounded rectangle: center vertex + perimeter vertices
            constexpr int SEGMENTS = 8; // segments per corner quarter-circle

            float cx = (x0 + x1) * 0.5f;
            float cy = (y0 + y1) * 0.5f;

            // Center vertex
            vertices.push_back({{cx, cy}, panel.color});
            uint32_t centerIdx = vertexOffset;
            vertexOffset++;

            // 4 corners: top-left, top-right, bottom-right, bottom-left
            // Each corner center and start/end angles
            struct Corner
            {
                float cx, cy;
                float startAngle;
            };
            Corner corners[4] = {
                {x0 + r, y0 + r, static_cast<float>(M_PI)},        // top-left
                {x1 - r, y0 + r, static_cast<float>(M_PI) * 1.5f}, // top-right
                {x1 - r, y1 - r, 0.0f},                            // bottom-right
                {x0 + r, y1 - r, static_cast<float>(M_PI) * 0.5f}, // bottom-left
            };

            // Generate perimeter vertices: for each corner, SEGMENTS+1 points
            uint32_t perimStart = vertexOffset;
            for (int c = 0; c < 4; c++)
            {
                for (int s = 0; s <= SEGMENTS; s++)
                {
                    float angle = corners[c].startAngle +
                                  (static_cast<float>(M_PI) * 0.5f) *
                                      (static_cast<float>(s) / static_cast<float>(SEGMENTS));
                    float px = corners[c].cx + r * std::cos(angle);
                    float py = corners[c].cy + r * std::sin(angle);
                    vertices.push_back({{px, py}, panel.color});
                    vertexOffset++;
                }
            }

            // Fan triangles from center to consecutive perimeter vertices
            uint32_t totalPerim = 4 * (SEGMENTS + 1);
            for (uint32_t i = 0; i < totalPerim; i++)
            {
                uint32_t next = (i + 1) % totalPerim;
                indices.push_back(centerIdx);
                indices.push_back(perimStart + i);
                indices.push_back(perimStart + next);
            }
        }
    }

    indexCount = static_cast<uint32_t>(indices.size());

    if (indexCount == 0)
        return;

    glBindVertexArray(vao);

    if (vbo == 0)
        glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 vertices.size() * sizeof(UIVertex),
                 vertices.data(),
                 GL_DYNAMIC_DRAW);

    if (ibo == 0)
        glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 indices.size() * sizeof(uint32_t),
                 indices.data(),
                 GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                          sizeof(UIVertex),
                          (void *)offsetof(UIVertex, position));
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE,
                          sizeof(UIVertex),
                          (void *)offsetof(UIVertex, color));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
    dirty = false;
}

void UIRenderer::Render()
{
    if (panels.empty())
        return;

    if (dirty)
        BuildMesh();

    if (indexCount == 0)
        return;

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    shader.Use();
    shader.SetMat4("uProjection", projection);

    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

bool UIRenderer::HitTest(float pixelX, float pixelY) const
{
    if (grid.cellSizeX <= 0.0f || grid.cellSizeY <= 0.0f)
        return false;

    float cellX = pixelX / grid.cellSizeX;
    float cellY = pixelY / grid.cellSizeY;

    for (const auto &panel : panels)
    {
        if (!panel.visible)
            continue;

        if (cellX >= panel.col && cellX <= panel.col + panel.colSpan &&
            cellY >= panel.row && cellY <= panel.row + panel.rowSpan)
        {
            return true;
        }
    }
    return false;
}

void UIRenderer::Shutdown()
{
    shader.Delete();
    if (vbo)
        glDeleteBuffers(1, &vbo);
    if (ibo)
        glDeleteBuffers(1, &ibo);
    if (vao)
        glDeleteVertexArrays(1, &vao);
    vbo = ibo = vao = 0;
    indexCount = 0;
}
