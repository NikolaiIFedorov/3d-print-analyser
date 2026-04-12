#include "UIRenderer.hpp"
#include "rendering/color.hpp"
#include "utils/log.hpp"
#include "imgui.h"
#include <algorithm>
#include <cmath>

static constexpr float SPLITTER_HEIGHT = 0.125f; // splitter thickness in cells

UIRenderer::UIRenderer(SDL_Window *window, const std::string &fontPath)
    : window(window)
{
    if (!InitializeShaders())
    {
        LOG_FALSE("Failed to initialize UI shaders");
        return;
    }

    glGenVertexArrays(1, &vao);

    if (!textRenderer.Init(fontPath, 48))
    {
        LOG_FALSE("Failed to initialize TextRenderer");
    }

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
      textRenderer(std::move(other.textRenderer)),
      window(other.window),
      dirty(other.dirty)
{
    other.vao = other.vbo = other.ibo = 0;
    other.indexCount = 0;
    other.window = nullptr;
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
        textRenderer = std::move(other.textRenderer);
        window = other.window;
        dirty = other.dirty;
        other.vao = other.vbo = other.ibo = 0;
        other.indexCount = 0;
        other.window = nullptr;
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

    float displayScale = window ? SDL_GetWindowDisplayScale(window) : 1.0f;
    grid.Update(width, height, displayScale);

    textRenderer.SetProjection(projection);

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

void UIRenderer::SetSectionValue(const std::string &panelId, const std::string &sectionId, const std::vector<SectionLine> &values)
{
    Panel *panel = GetPanel(panelId);
    if (!panel)
        return;

    for (auto &section : panel->sections)
    {
        if (section.id == sectionId)
        {
            section.values = values;
            dirty = true;
            return;
        }
        for (auto &content : section.sections)
        {
            if (content.id == sectionId)
            {
                content.values = values;
                dirty = true;
                return;
            }
        }
    }
}

void UIRenderer::SetSectionVisible(const std::string &panelId, const std::string &sectionId, bool visible)
{
    Panel *panel = GetPanel(panelId);
    if (!panel)
        return;

    for (auto &section : panel->sections)
    {
        if (section.id == sectionId && section.visible != visible)
        {
            section.visible = visible;
            dirty = true;
            return;
        }
        for (auto &content : section.sections)
        {
            if (content.id == sectionId && content.visible != visible)
            {
                content.visible = visible;
                dirty = true;
                return;
            }
        }
    }
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

        // Auto-width: fit content when width is not explicitly set
        if (!w && !right)
        {
            float localCell = grid.cellSizeX * PanelGrid::LOCAL_CELL_RATIO;
            float textScale = localCell / textRenderer.GetLineHeight(1.0f) * 1.4f;
            float textWidthPx = textRenderer.MeasureWidth(panel.id, textScale);
            float textWidthCells = textWidthPx / grid.cellSizeX;
            float headerWidth = 2.0f * panel.padding + textWidthCells;

            // Sections expand width only for vertical panels;
            // horizontal panels expand later after both axes are known.
            w = headerWidth;

            // Vertical sections: widen panel to fit label and value lines
            bool willBeVertical = !(panel.leftAnchor.has_value() && panel.rightAnchor.has_value());
            if (willBeVertical)
            {
                float wGAP = panel.padding;
                for (const auto &section : panel.sections)
                {
                    if (!section.visible)
                        continue;
                    bool isSubPanel = section.color.a > 0.0f;
                    float subExtra = isSubPanel ? 2.0f * wGAP : 0.0f;
                    float secLabelW = textRenderer.MeasureWidth(section.id, textScale) / grid.cellSizeX;
                    float secWidth = 2.0f * wGAP + secLabelW + subExtra;
                    for (const auto &line : section.values)
                    {
                        float lineW = textRenderer.MeasureWidth(line.prefix + line.text, textScale) / grid.cellSizeX;
                        float lineWidth = 2.0f * wGAP + lineW + subExtra;
                        secWidth = std::max(secWidth, lineWidth);
                    }
                    for (const auto &content : section.sections)
                    {
                        if (!content.visible)
                            continue;
                        float cLabelW = textRenderer.MeasureWidth(content.id, textScale) / grid.cellSizeX;
                        float cWidth = 2.0f * wGAP + cLabelW + subExtra;
                        for (const auto &cLine : content.values)
                        {
                            float cLineW = textRenderer.MeasureWidth(cLine.prefix + cLine.text, textScale) / grid.cellSizeX;
                            float cLineWidth = 2.0f * wGAP + cLineW + subExtra;
                            cWidth = std::max(cWidth, cLineWidth);
                        }
                        secWidth = std::max(secWidth, cWidth);
                    }
                    w = std::max(*w, secWidth);
                }
            }
        }

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

        // Auto-height: fit content when height is not explicitly set
        if (!h && !bottom)
        {
            float localCell = grid.cellSizeX * PanelGrid::LOCAL_CELL_RATIO;
            float textScale = localCell / textRenderer.GetLineHeight(1.0f) * 1.4f;
            float textHeightPx = textRenderer.GetMaxBearingY(textScale);
            float textHeightCells = textHeightPx / grid.cellSizeY;
            float headerHeight = 2.0f * panel.padding + textHeightCells;

            // Sections expand height only for vertical panels;
            // horizontal panels leave height as header-only.
            h = headerHeight;
        }

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

        // Compute local grid from resolved pixel rect
        float px = grid.ToPixelsX(panel.col);
        float py = grid.ToPixelsY(panel.row);
        float pw = grid.ToPixelsX(panel.colSpan);
        float ph = grid.ToPixelsY(panel.rowSpan);
        panel.localGrid.Update(px, py, pw, ph, grid.cellSizeX, panel.padding);

        // Resolve child sections — direction depends on panel aspect ratio
        if (!panel.sections.empty())
        {
            float localCell = grid.cellSizeX * PanelGrid::LOCAL_CELL_RATIO;
            float textScale = localCell / textRenderer.GetLineHeight(1.0f) * 1.4f;
            float textHeightPx = textRenderer.GetMaxBearingY(textScale);
            float textHeightCells = textHeightPx / grid.cellSizeY;
            float GAP = panel.padding;                       // universal spacing = padding
            float lineStep = GAP + textHeightCells;          // per-line step: gap + text
            float itemHeight = 2.0f * GAP + textHeightCells; // full item: pad + text + pad

            float textWidthPx = textRenderer.MeasureWidth(panel.id, textScale);
            float textWidthCells = textWidthPx / grid.cellSizeX;
            float sectionWidth = 2.0f * GAP + textWidthCells;

            float secPadding = Panel::PaddingForLayer(1);

            bool horizontal = panel.leftAnchor.has_value() && panel.rightAnchor.has_value();

            if (horizontal)
            {
                // Stack sections rightward; expand panel width to fit
                float headerWidth = sectionWidth;
                float totalWidth = headerWidth;
                for (size_t i = 0; i < panel.sections.size(); i++)
                {
                    if (!panel.sections[i].visible)
                        continue;
                    float secTextW = textRenderer.MeasureWidth(panel.sections[i].id, textScale);
                    float secW = 2.0f * secPadding + secTextW / grid.cellSizeX;
                    totalWidth += (panel.sections[i].showSplitter ? SPLITTER_HEIGHT : 0.0f) + secW;
                }

                // Only expand if auto-sized (no right anchor and no fixed width)
                if (!panel.rightAnchor && !panel.width)
                    panel.colSpan = std::max(panel.colSpan, totalWidth);

                float currentCol = panel.col + headerWidth;
                for (auto &section : panel.sections)
                {
                    if (!section.visible)
                        continue;
                    currentCol += section.showSplitter ? SPLITTER_HEIGHT : 0.0f;
                    float secTextW = textRenderer.MeasureWidth(section.id, textScale);
                    float secW = 2.0f * secPadding + secTextW / grid.cellSizeX;

                    section.col = currentCol;
                    section.colSpan = secW;
                    section.row = panel.row;
                    section.rowSpan = panel.rowSpan;

                    float spx = grid.ToPixelsX(section.col);
                    float spy = grid.ToPixelsY(section.row);
                    float spw = grid.ToPixelsX(section.colSpan);
                    float sph = grid.ToPixelsY(section.rowSpan);
                    section.localGrid.Update(spx, spy, spw, sph, grid.cellSizeX, panel.padding);

                    currentCol += secW;
                }
            }
            else
            {
                // Stack sections downward; expand panel height to fit
                float totalHeight = itemHeight; // header (includes own top+bottom padding)

                // Compute height of a leaf item (no children)
                auto leafHeight = [&](const Panel &item) -> float
                {
                    float h = itemHeight;
                    if (!item.values.empty())
                    {
                        float valLines = static_cast<float>(item.values.size());
                        h = item.showLabel
                                ? itemHeight + valLines * lineStep
                                : GAP + valLines * lineStep;
                    }
                    return h;
                };

                // Compute total height of a section (including contents)
                auto computeSecHeight = [&](const Panel &section) -> float
                {
                    if (section.sections.empty())
                        return leafHeight(section);
                    bool isSubPanel = section.color.a > 0.0f;
                    float subGap = isSubPanel ? GAP : 0.0f;
                    float splGap = (isSubPanel && section.showLabel) ? GAP : 0.0f;
                    float h = section.showLabel ? itemHeight : 0.0f;
                    for (const auto &content : section.sections)
                    {
                        if (!content.visible)
                            continue;
                        h += leafHeight(content);
                    }
                    return h + 2.0f * subGap + splGap;
                };

                for (const auto &section : panel.sections)
                {
                    if (!section.visible)
                        continue;
                    totalHeight += computeSecHeight(section);
                }

                if (!panel.bottomAnchor && !panel.height)
                    panel.rowSpan = std::max(panel.rowSpan, totalHeight);

                float currentRow = panel.row + itemHeight;
                for (auto &section : panel.sections)
                {
                    if (!section.visible)
                        continue;
                    float secH = computeSecHeight(section);

                    section.col = panel.col;
                    section.colSpan = panel.colSpan;
                    section.row = currentRow;
                    section.rowSpan = secH;

                    float spx = grid.ToPixelsX(section.col);
                    float spy = grid.ToPixelsY(section.row);
                    float spw = grid.ToPixelsX(section.colSpan);
                    float sph = grid.ToPixelsY(section.rowSpan);
                    section.localGrid.Update(spx, spy, spw, sph, grid.cellSizeX, panel.padding);

                    // Place contents within section
                    if (!section.sections.empty())
                    {
                        bool isSubPanel = section.color.a > 0.0f;
                        float subGap = isSubPanel ? GAP : 0.0f;
                        float splGap = (isSubPanel && section.showLabel) ? GAP : 0.0f;
                        float contentRow = currentRow + (section.showLabel ? itemHeight : 0.0f) + subGap + splGap;
                        float secPad = isSubPanel ? section.padding : panel.padding;
                        // For sub-panels, inset content col/colSpan to the background bounds
                        float cCol = isSubPanel ? section.col + section.padding : section.col;
                        float cColSpan = isSubPanel ? section.colSpan - 2.0f * section.padding : section.colSpan;
                        for (auto &content : section.sections)
                        {
                            if (!content.visible)
                                continue;
                            float cH = leafHeight(content);
                            content.col = cCol;
                            content.colSpan = cColSpan;
                            content.row = contentRow;
                            content.rowSpan = cH;

                            float cpx = grid.ToPixelsX(content.col);
                            float cpy = grid.ToPixelsY(content.row);
                            float cpw = grid.ToPixelsX(content.colSpan);
                            float cph = grid.ToPixelsY(content.rowSpan);
                            content.localGrid.Update(cpx, cpy, cpw, cph, grid.cellSizeX, secPad);

                            contentRow += cH;
                        }
                    }

                    currentRow += secH;
                }
            }

            // Recompute parent local grid after potential size expansion
            px = grid.ToPixelsX(panel.col);
            py = grid.ToPixelsY(panel.row);
            pw = grid.ToPixelsX(panel.colSpan);
            ph = grid.ToPixelsY(panel.rowSpan);
            panel.localGrid.Update(px, py, pw, ph, grid.cellSizeX, panel.padding);
        }
    }
}

void UIRenderer::ComputeMinGridSize()
{
    // Simulate a resolve pass using each panel's minimum dimensions
    // to find the smallest screen extent that fits all panels.
    const float gap = UIGrid::GAP;
    float maxRight = 0.0f;
    float maxBottom = 0.0f;

    // Temporary resolved positions for min-size calculation
    struct Resolved
    {
        float col, row, colSpan, rowSpan;
    };
    std::vector<Resolved> resolved;
    resolved.reserve(panels.size());

    auto resolveEdgeMin = [&](const PanelAnchor &anchor, float minExtentX, float minExtentY) -> float
    {
        if (!anchor.panel)
        {
            switch (anchor.edge)
            {
            case PanelAnchor::Right:
                return minExtentX - gap;
            case PanelAnchor::Bottom:
                return minExtentY - gap;
            default:
                return gap;
            }
        }

        // Find the resolved entry for the referenced panel
        for (size_t i = 0; i < panels.size(); i++)
        {
            if (&panels[i] == anchor.panel)
            {
                const auto &r = resolved[i];
                switch (anchor.edge)
                {
                case PanelAnchor::Left:
                    return r.col - gap;
                case PanelAnchor::Right:
                    return r.col + r.colSpan + gap;
                case PanelAnchor::Top:
                    return r.row - gap;
                case PanelAnchor::Bottom:
                    return r.row + r.rowSpan + gap;
                }
            }
        }
        return 0.0f;
    };

    // We don't know the final min extent yet, so we iterate:
    // use a large initial extent for screen-edge anchors, then shrink to fit.
    // For panels anchored to screen right/bottom, use minWidth/minHeight of
    // left-anchored panels to determine position, then the screen-edge panels
    // collapse to fit.

    // First pass: resolve panels using their min sizes where applicable
    float extentX = 1000.0f; // large initial guess
    float extentY = 1000.0f;

    for (const auto &panel : panels)
    {
        Resolved r{};

        // Horizontal
        std::optional<float> left, right;
        if (panel.leftAnchor)
            left = resolveEdgeMin(*panel.leftAnchor, extentX, extentY);
        if (panel.rightAnchor)
            right = resolveEdgeMin(*panel.rightAnchor, extentX, extentY);

        // Use minWidth for stretching panels, width for fixed panels
        float w = 0.0f;
        if (panel.leftAnchor && panel.rightAnchor)
            w = panel.minWidth.value_or(panel.width.value_or(0.0f));
        else if (panel.width)
            w = *panel.width;
        else
        {
            // Auto-width: fit header content
            float localCell = grid.cellSizeX * PanelGrid::LOCAL_CELL_RATIO;
            float textScale = localCell / textRenderer.GetLineHeight(1.0f) * 1.4f;
            float textWidthPx = textRenderer.MeasureWidth(panel.id, textScale);
            float textWidthCells = textWidthPx / grid.cellSizeX;
            w = 2.0f * panel.padding + textWidthCells;
        }

        if (left)
        {
            r.col = *left;
            r.colSpan = w;
        }
        else if (right)
        {
            r.col = *right - w;
            r.colSpan = w;
        }

        // Vertical
        std::optional<float> top, bottom;
        if (panel.topAnchor)
            top = resolveEdgeMin(*panel.topAnchor, extentX, extentY);
        if (panel.bottomAnchor)
            bottom = resolveEdgeMin(*panel.bottomAnchor, extentX, extentY);

        float h = 0.0f;
        if (panel.topAnchor && panel.bottomAnchor)
            h = panel.minHeight.value_or(panel.height.value_or(0.0f));
        else if (panel.height)
            h = *panel.height;
        else
        {
            // Auto-height: fit header content
            float localCell = grid.cellSizeX * PanelGrid::LOCAL_CELL_RATIO;
            float textScale = localCell / textRenderer.GetLineHeight(1.0f) * 1.4f;
            float textHeightPx = textRenderer.GetMaxBearingY(textScale);
            float textHeightCells = textHeightPx / grid.cellSizeY;
            h = 2.0f * panel.padding + textHeightCells;
        }

        if (top)
        {
            r.row = *top;
            r.rowSpan = h;
        }
        else if (bottom)
        {
            r.row = *bottom - h;
            r.rowSpan = h;
        }

        // Expand for sections based on direction
        if (!panel.sections.empty())
        {
            bool isHorizontal = panel.leftAnchor.has_value() && panel.rightAnchor.has_value();
            float localCell = grid.cellSizeX * PanelGrid::LOCAL_CELL_RATIO;
            float textScale = localCell / textRenderer.GetLineHeight(1.0f) * 1.4f;

            if (isHorizontal)
            {
                float totalWidth = r.colSpan;
                for (const auto &sec : panel.sections)
                {
                    if (!sec.visible)
                        continue;
                    float secTextW = textRenderer.MeasureWidth(sec.id, textScale);
                    float secW = 2.0f * sec.padding + secTextW / grid.cellSizeX;
                    totalWidth += (sec.showSplitter ? SPLITTER_HEIGHT : 0.0f) + secW;
                }
                if (!panel.rightAnchor && !panel.width)
                    r.colSpan = totalWidth;
            }
            else
            {
                float textHeightPx = textRenderer.GetMaxBearingY(textScale);
                float textHeightCells = textHeightPx / grid.cellSizeY;
                float minGAP = panel.padding;
                float minItemH = 2.0f * minGAP + textHeightCells;
                float totalHeight = r.rowSpan;
                for (size_t i = 0; i < panel.sections.size(); i++)
                {
                    if (!panel.sections[i].visible)
                        continue;
                    bool isSubPanel = panel.sections[i].color.a > 0.0f;
                    float subExtra = isSubPanel ? 3.0f * minGAP : 0.0f;
                    totalHeight += minItemH + subExtra;
                }
                if (!panel.bottomAnchor && !panel.height)
                    r.rowSpan = totalHeight;
            }
        }

        resolved.push_back(r);

        float panelRight = r.col + r.colSpan + gap;
        float panelBottom = r.row + r.rowSpan + gap;
        if (panelRight > maxRight)
            maxRight = panelRight;
        if (panelBottom > maxBottom)
            maxBottom = panelBottom;
    }

    grid.minColumns = maxRight;
    grid.minRows = maxBottom;
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

        // Panel background geometry (skip if transparent)
        if (panel.color.a > 0.0f)
        {
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
        } // end if (panel.color.a > 0.0f)

        // Generate splitter geometry between sections (inset halfway into padding)
        bool horizontal = panel.leftAnchor.has_value() && panel.rightAnchor.has_value();
        float halfPadPx = grid.ToPixelsX(panel.padding) * 0.5f;
        for (const auto &section : panel.sections)
        {
            if (!section.visible || !section.showSplitter)
                continue;
            float rsx0, rsy0, rsx1, rsy1;
            if (horizontal)
            {
                // Vertical splitter: left of section, inset top/bottom
                float halfSpl = SPLITTER_HEIGHT * 0.5f;
                rsx0 = grid.ToPixelsX(section.col - halfSpl);
                rsx1 = grid.ToPixelsX(section.col + halfSpl);
                rsy0 = y0 + halfPadPx;
                rsy1 = y1 - halfPadPx;
            }
            else
            {
                // Horizontal splitter: above section, centered in gap
                float halfSpl = SPLITTER_HEIGHT * 0.5f;
                rsx0 = x0 + halfPadPx;
                rsx1 = x1 - halfPadPx;
                rsy0 = grid.ToPixelsY(section.row - halfSpl);
                rsy1 = grid.ToPixelsY(section.row + halfSpl);
            }

            // Snap splitter rect to whole pixels for clean border radius
            rsx0 = std::round(rsx0);
            rsy0 = std::round(rsy0);
            rsx1 = std::round(rsx1);
            rsy1 = std::round(rsy1);

            glm::vec4 splitterColor = Color::GetUI(2);
            float sr = std::min(rsx1 - rsx0, rsy1 - rsy0) * 0.5f;

            constexpr int SEGS = 8;
            float scx = (rsx0 + rsx1) * 0.5f;
            float scy = (rsy0 + rsy1) * 0.5f;

            vertices.push_back({{scx, scy}, splitterColor});
            uint32_t sCenterIdx = vertexOffset;
            vertexOffset++;

            struct Corner
            {
                float cx, cy;
                float startAngle;
            };
            Corner sCorners[4] = {
                {rsx0 + sr, rsy0 + sr, static_cast<float>(M_PI)},
                {rsx1 - sr, rsy0 + sr, static_cast<float>(M_PI) * 1.5f},
                {rsx1 - sr, rsy1 - sr, 0.0f},
                {rsx0 + sr, rsy1 - sr, static_cast<float>(M_PI) * 0.5f},
            };

            uint32_t sPerimStart = vertexOffset;
            for (int c = 0; c < 4; c++)
            {
                for (int s = 0; s <= SEGS; s++)
                {
                    float angle = sCorners[c].startAngle +
                                  (static_cast<float>(M_PI) * 0.5f) *
                                      (static_cast<float>(s) / static_cast<float>(SEGS));
                    float spx = sCorners[c].cx + sr * std::cos(angle);
                    float spy = sCorners[c].cy + sr * std::sin(angle);
                    vertices.push_back({{spx, spy}, splitterColor});
                    vertexOffset++;
                }
            }

            uint32_t sTotalPerim = 4 * (SEGS + 1);
            for (uint32_t i = 0; i < sTotalPerim; i++)
            {
                uint32_t next = (i + 1) % sTotalPerim;
                indices.push_back(sCenterIdx);
                indices.push_back(sPerimStart + i);
                indices.push_back(sPerimStart + next);
            }
        }

        // Generate section and content background geometry
        auto generateBackground = [&](const Panel &item)
        {
            float inset = (!item.sections.empty()) ? item.padding : panel.padding;
            float padPxH = grid.ToPixelsX(inset);
            float padPxV = grid.ToPixelsY(inset);
            float sx0 = grid.ToPixelsX(item.col) + padPxH;
            float sy0 = grid.ToPixelsY(item.row) + padPxV;
            float sx1 = grid.ToPixelsX(item.col + item.colSpan) - padPxH;
            float sy1 = grid.ToPixelsY(item.row + item.rowSpan) - padPxV;

            float sr = std::min(grid.cellSizeX, grid.cellSizeY) * item.borderRadius;
            float maxSR = std::min((sx1 - sx0), (sy1 - sy0)) * 0.5f;
            if (sr > maxSR)
                sr = maxSR;

            constexpr int SEGS = 8;
            float scx = (sx0 + sx1) * 0.5f;
            float scy = (sy0 + sy1) * 0.5f;

            vertices.push_back({{scx, scy}, item.color});
            uint32_t secCenterIdx = vertexOffset;
            vertexOffset++;

            struct Corner
            {
                float cx, cy, startAngle;
            };
            Corner secCorners[4] = {
                {sx0 + sr, sy0 + sr, static_cast<float>(M_PI)},
                {sx1 - sr, sy0 + sr, static_cast<float>(M_PI) * 1.5f},
                {sx1 - sr, sy1 - sr, 0.0f},
                {sx0 + sr, sy1 - sr, static_cast<float>(M_PI) * 0.5f},
            };

            uint32_t secPerimStart = vertexOffset;
            for (int c = 0; c < 4; c++)
            {
                for (int s = 0; s <= SEGS; s++)
                {
                    float angle = secCorners[c].startAngle +
                                  (static_cast<float>(M_PI) * 0.5f) *
                                      (static_cast<float>(s) / static_cast<float>(SEGS));
                    float spx = secCorners[c].cx + sr * std::cos(angle);
                    float spy = secCorners[c].cy + sr * std::sin(angle);
                    vertices.push_back({{spx, spy}, item.color});
                    vertexOffset++;
                }
            }

            uint32_t secTotalPerim = 4 * (SEGS + 1);
            for (uint32_t i = 0; i < secTotalPerim; i++)
            {
                uint32_t next = (i + 1) % secTotalPerim;
                indices.push_back(secCenterIdx);
                indices.push_back(secPerimStart + i);
                indices.push_back(secPerimStart + next);
            }
        };

        for (const auto &section : panel.sections)
        {
            if (!section.visible)
                continue;
            if (section.color.a > 0.0f)
                generateBackground(section);

            // Draw splitters between contents inside colored (sub-panel) sections
            if (section.color.a > 0.0f && section.showLabel && !section.sections.empty())
            {
                // Find the first visible content to place a splitter above it
                for (const auto &content : section.sections)
                {
                    if (!content.visible)
                        continue;

                    float secPadPxH = grid.ToPixelsX(section.padding);
                    float secBgX0 = grid.ToPixelsX(section.col) + secPadPxH;
                    float secBgX1 = grid.ToPixelsX(section.col + section.colSpan) - secPadPxH;
                    float secHalfPad = secPadPxH * 0.5f;
                    float rsx0 = secBgX0 + secHalfPad;
                    float rsx1 = secBgX1 - secHalfPad;
                    float halfSpl = SPLITTER_HEIGHT * 0.5f;
                    float rsy0 = grid.ToPixelsY(content.row - halfSpl);
                    float rsy1 = grid.ToPixelsY(content.row + halfSpl);

                    // Snap to whole pixels for clean border radius
                    rsx0 = std::round(rsx0);
                    rsy0 = std::round(rsy0);
                    rsx1 = std::round(rsx1);
                    rsy1 = std::round(rsy1);

                    glm::vec4 splColor = Color::GetUI(3);
                    float sr = std::min(rsx1 - rsx0, rsy1 - rsy0) * 0.5f;
                    constexpr int SEGS = 8;
                    float scx = (rsx0 + rsx1) * 0.5f;
                    float scy = (rsy0 + rsy1) * 0.5f;
                    vertices.push_back({{scx, scy}, splColor});
                    uint32_t cIdx = vertexOffset++;
                    struct Corner
                    {
                        float cx, cy, startAngle;
                    };
                    Corner cs[4] = {
                        {rsx0 + sr, rsy0 + sr, static_cast<float>(M_PI)},
                        {rsx1 - sr, rsy0 + sr, static_cast<float>(M_PI) * 1.5f},
                        {rsx1 - sr, rsy1 - sr, 0.0f},
                        {rsx0 + sr, rsy1 - sr, static_cast<float>(M_PI) * 0.5f},
                    };
                    uint32_t pStart = vertexOffset;
                    for (int c = 0; c < 4; c++)
                        for (int s = 0; s <= SEGS; s++)
                        {
                            float angle = cs[c].startAngle + (static_cast<float>(M_PI) * 0.5f) * (static_cast<float>(s) / static_cast<float>(SEGS));
                            vertices.push_back({{cs[c].cx + sr * std::cos(angle), cs[c].cy + sr * std::sin(angle)}, splColor});
                            vertexOffset++;
                        }
                    uint32_t total = 4 * (SEGS + 1);
                    for (uint32_t i = 0; i < total; i++)
                    {
                        indices.push_back(cIdx);
                        indices.push_back(pStart + i);
                        indices.push_back(pStart + (i + 1) % total);
                    }
                    break; // only one splitter, between header and first content
                }
            }

            for (const auto &content : section.sections)
            {
                if (!content.visible || content.color.a <= 0.0f)
                    continue;
                generateBackground(content);
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

    // Render panel titles using each panel's local grid
    for (const auto &panel : panels)
    {
        if (!panel.visible || panel.id.empty())
            continue;

        const auto &lg = panel.localGrid;
        float textScale = lg.cellSizeY / textRenderer.GetLineHeight(1.0f) * 1.4f;

        // Header is row 0 of the local grid, inset by padding (= border radius)
        float px = lg.ToPixelsX(lg.padding);
        float py = lg.ToPixelsY(lg.padding) + textRenderer.GetMaxBearingY(textScale);
        textRenderer.RenderText(panel.id, px, py, textScale, Color::GetUIText(1));

        // Helper to render a section/content item
        auto renderItem = [&](const Panel &item, const Panel &parent)
        {
            if (item.imguiContent)
            {
                float padPxH = grid.ToPixelsX(parent.padding);
                float padPxV = grid.ToPixelsY(parent.padding);
                float sx0 = grid.ToPixelsX(item.col) + padPxH;
                float sy0 = grid.ToPixelsY(item.row) + padPxV;
                float sx1 = grid.ToPixelsX(item.col + item.colSpan) - padPxH;
                float sy1 = grid.ToPixelsY(item.row + item.rowSpan) - padPxV;
                float sw = sx1 - sx0;
                float sh = sy1 - sy0;

                ImGui::SetNextWindowPos(ImVec2(sx0, sy0));
                ImGui::SetNextWindowSize(ImVec2(sw, sh));
                std::string winId = "##" + panel.id + "_" + item.id;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
                if (ImGui::Begin(winId.c_str(), nullptr,
                                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_AlwaysAutoResize))
                {
                    item.imguiContent(sw, sh);
                }
                ImGui::End();
                ImGui::PopStyleVar(2);
                return;
            }

            const auto &slg = item.localGrid;
            float sTextScale = slg.cellSizeY / textRenderer.GetLineHeight(1.0f) * 1.4f;
            float bearingY = textRenderer.GetMaxBearingY(sTextScale);
            float lineStepPx = panel.padding * grid.cellSizeY + bearingY; // matches layout lineStep

            if (item.color.a > 0.0f)
            {
                float inset = (!item.sections.empty()) ? item.padding : panel.padding;
                float bgX0 = grid.ToPixelsX(item.col) + grid.ToPixelsX(inset);
                float bgY0 = grid.ToPixelsY(item.row) + grid.ToPixelsY(inset);
                float bgX1 = grid.ToPixelsX(item.col + item.colSpan) - grid.ToPixelsX(inset);
                float bgY1 = grid.ToPixelsY(item.row + item.rowSpan) - grid.ToPixelsY(inset);

                if (!item.id.empty() && item.showLabel)
                {
                    if (!item.sections.empty())
                    {
                        // Sub-panel style: top-left header
                        float spx = bgX0 + grid.ToPixelsX(item.padding);
                        float spy = bgY0 + grid.ToPixelsY(item.padding) + bearingY;
                        textRenderer.RenderText(item.id, spx, spy, sTextScale, Color::GetUIText(1));
                    }
                    else
                    {
                        // Button style: centered
                        float tw = textRenderer.MeasureWidth(item.id, sTextScale);
                        float spx = bgX0 + (bgX1 - bgX0 - tw) * 0.5f;
                        float spy = bgY0 + (bgY1 - bgY0 + bearingY) * 0.5f;
                        textRenderer.RenderText(item.id, spx, spy, sTextScale, Color::GetUIText(1));
                    }
                }

                float valueStart = item.showLabel ? 1.0f : 0.0f;
                for (size_t i = 0; i < item.values.size(); i++)
                {
                    const auto &line = item.values[i];
                    float tw = textRenderer.MeasureWidth(line.prefix + line.text, sTextScale);
                    float vpx = bgX0 + (bgX1 - bgX0 - tw) * 0.5f;
                    float vpy = bgY0 + (bgY1 - bgY0 + bearingY) * 0.5f + lineStepPx * (valueStart + static_cast<float>(i));
                    if (!line.prefix.empty())
                    {
                        textRenderer.RenderText(line.prefix, vpx, vpy, sTextScale, line.prefixColor);
                        vpx += textRenderer.MeasureWidth(line.prefix, sTextScale);
                    }
                    if (!line.text.empty())
                        textRenderer.RenderText(line.text, vpx, vpy, sTextScale, Color::GetUIText(1));
                }
            }
            else
            {
                if (!item.id.empty() && item.showLabel)
                {
                    float spx = slg.ToPixelsX(slg.padding);
                    float spy = slg.ToPixelsY(slg.padding) + bearingY;
                    textRenderer.RenderText(item.id, spx, spy, sTextScale, Color::GetUIText(1));
                }

                float valueStart = item.showLabel ? 1.0f : 0.0f;
                for (size_t i = 0; i < item.values.size(); i++)
                {
                    const auto &line = item.values[i];
                    float vpx = slg.ToPixelsX(slg.padding);
                    float vpy = slg.ToPixelsY(slg.padding) + bearingY + lineStepPx * (valueStart + static_cast<float>(i));

                    float btnX = vpx;
                    float btnY = vpy - bearingY;
                    float btnW = (slg.columns - 2.0f * slg.padding) * slg.cellSizeX;
                    float btnH = bearingY;
                    ImGui::SetNextWindowPos(ImVec2(btnX, btnY));
                    ImGui::SetNextWindowSize(ImVec2(btnW, btnH));
                    std::string winId = "##val_" + panel.id + "_" + item.id + "_" + std::to_string(i);
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
                    if (line.onClick)
                    {
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, btnH * 0.3f);
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.15f, 0.15f, 0.5f));
                        if (pixelImFont)
                            ImGui::PushFont(pixelImFont);
                    }
                    if (ImGui::Begin(winId.c_str(), nullptr,
                                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings))
                    {
                        if (line.onClick)
                        {
                            if (ImGui::Button(("##btn" + std::to_string(i)).c_str(), ImVec2(btnW, btnH)))
                                line.onClick();
                            if (ImGui::IsItemHovered())
                                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        }

                        float pad = ImGui::GetStyle().FramePadding.x;
                        float ty = btnY + (btnH - ImGui::GetFontSize()) * 0.5f;
                        ImDrawList *dl = ImGui::GetWindowDrawList();
                        float tx = btnX + pad;
                        if (!line.prefix.empty())
                        {
                            ImU32 pc = ImGui::GetColorU32(ImVec4(line.prefixColor.r, line.prefixColor.g, line.prefixColor.b, line.prefixColor.a));
                            dl->AddText(ImVec2(tx, ty), pc, line.prefix.c_str());
                            tx += ImGui::CalcTextSize(line.prefix.c_str()).x;
                        }
                        if (!line.text.empty())
                        {
                            glm::vec4 tc = Color::GetUIText(1);
                            ImU32 textCol = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
                            dl->AddText(ImVec2(tx, ty), textCol, line.text.c_str());
                        }
                    }
                    ImGui::End();
                    if (line.onClick)
                    {
                        if (pixelImFont)
                            ImGui::PopFont();
                        ImGui::PopStyleColor(3);
                        ImGui::PopStyleVar(1);
                    }
                    ImGui::PopStyleVar(2);
                }
            }
        };

        // Render section labels, values, and contents
        for (const auto &section : panel.sections)
        {
            if (!section.visible)
                continue;
            renderItem(section, panel);
            const Panel &contentParent = (section.color.a > 0.0f) ? section : panel;
            for (const auto &content : section.sections)
            {
                if (!content.visible)
                    continue;
                renderItem(content, contentParent);
            }
        }
    }

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
        if (!panel.visible || panel.color.a <= 0.0f)
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
