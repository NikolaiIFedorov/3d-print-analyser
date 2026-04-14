#include "UIRenderer.hpp"
#include "rendering/color.hpp"
#include "utils/log.hpp"
#include "imgui.h"
#include <algorithm>
#include <cmath>

static constexpr float SPLITTER_HEIGHT = 0.125f; // splitter line thickness in cells
static constexpr float SPLITTER_PAD = 0.125f;    // padding above and below the splitter line
static constexpr float SPLITTER_TOTAL = SPLITTER_HEIGHT + 2.0f * SPLITTER_PAD;

TextMetrics UIRenderer::ComputeTextMetrics() const
{
    TextMetrics tm;
    tm.localCell = grid.cellSizeX * PanelGrid::LOCAL_CELL_RATIO;
    tm.textScale = tm.localCell / textRenderer.GetLineHeight(1.0f) * 1.4f;
    tm.textHeightCells = textRenderer.GetLineHeight(tm.textScale) / grid.cellSizeY;
    return tm;
}

void UIRenderer::EmitRoundedRect(std::vector<UIVertex> &vertices, std::vector<uint32_t> &indices,
                                 uint32_t &vertexOffset, float x0, float y0, float x1, float y1,
                                 float radius, glm::vec4 color)
{
    float maxR = std::min((x1 - x0), (y1 - y0)) * 0.5f;
    float r = std::min(radius, maxR);

    if (r <= 0.0f)
    {
        // Simple quad (no rounding)
        vertices.push_back({{x0, y0}, color});
        vertices.push_back({{x1, y0}, color});
        vertices.push_back({{x1, y1}, color});
        vertices.push_back({{x0, y1}, color});

        indices.push_back(vertexOffset + 0);
        indices.push_back(vertexOffset + 1);
        indices.push_back(vertexOffset + 2);
        indices.push_back(vertexOffset + 0);
        indices.push_back(vertexOffset + 2);
        indices.push_back(vertexOffset + 3);

        vertexOffset += 4;
        return;
    }

    constexpr int SEGMENTS = 8;

    float cx = (x0 + x1) * 0.5f;
    float cy = (y0 + y1) * 0.5f;

    vertices.push_back({{cx, cy}, color});
    uint32_t centerIdx = vertexOffset;
    vertexOffset++;

    struct Corner
    {
        float cx, cy, startAngle;
    };
    Corner corners[4] = {
        {x0 + r, y0 + r, static_cast<float>(M_PI)},        // top-left
        {x1 - r, y0 + r, static_cast<float>(M_PI) * 1.5f}, // top-right
        {x1 - r, y1 - r, 0.0f},                            // bottom-right
        {x0 + r, y1 - r, static_cast<float>(M_PI) * 0.5f}, // bottom-left
    };

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
            vertices.push_back({{px, py}, color});
            vertexOffset++;
        }
    }

    uint32_t totalPerim = 4 * (SEGMENTS + 1);
    for (uint32_t i = 0; i < totalPerim; i++)
    {
        uint32_t next = (i + 1) % totalPerim;
        indices.push_back(centerIdx);
        indices.push_back(perimStart + i);
        indices.push_back(perimStart + next);
    }
}

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

    auto search = [&](this auto &self, Panel &node) -> bool
    {
        if (node.id == sectionId)
        {
            node.values = values;
            dirty = true;
            return true;
        }
        for (auto &child : node.sections)
            if (self(child))
                return true;
        return false;
    };
    for (auto &section : panel->sections)
        if (search(section))
            return;
}

void UIRenderer::SetSectionVisible(const std::string &panelId, const std::string &sectionId, bool visible)
{
    Panel *panel = GetPanel(panelId);
    if (!panel)
        return;

    auto search = [&](this auto &self, Panel &node) -> bool
    {
        if (node.id == sectionId)
        {
            if (node.visible != visible)
            {
                node.visible = visible;
                dirty = true;
            }
            return true;
        }
        for (auto &child : node.sections)
            if (self(child))
                return true;
        return false;
    };
    for (auto &section : panel->sections)
        if (search(section))
            return;
}

void UIRenderer::ResolveAnchors()
{
    // --- Text metrics (computed once, reused throughout) ---
    TextMetrics tm = ComputeTextMetrics();
    float textScale = tm.textScale;
    float textHeightCells = tm.textHeightCells;

    // --- Compute content boxes bottom-up ---
    // Each element's box is: margin + padding + content + padding + margin (additive, no collapsing).

    auto computeBox = [&](this auto &self, Panel &item) -> void
    {
        float pad = item.padding;
        float mar = item.margin;
        item.lineGap = pad * Panel::LINE_GAP_RATIO;

        // Recurse into children first
        for (auto &child : item.sections)
            self(child);

        // --- Content height ---
        float contentH = 0.0f;
        float contentW = 0.0f;

        // Label contributes one text-height row, wrapped in childMar top+bottom (symmetric margin)
        if (item.showLabel)
        {
            float childMar = Panel::MarginForLayer(item.layer);
            contentH = childMar + textHeightCells + childMar;
            contentW = item.labelLeftInset + textRenderer.MeasureWidth(item.id, textScale) / grid.cellSizeX;
        }

        if (!item.sections.empty())
        {
            // Container: stack children additively.
            for (auto &child : item.sections)
            {
                if (!child.visible)
                    continue;
                contentH += child.box.outerHeight;
                contentW = std::max(contentW, child.box.outerWidth);
            }
        }
        else if (!item.values.empty())
        {
            // Leaf with value lines: label + lines stacked with lineGap
            float valLines = static_cast<float>(item.values.size());
            if (item.showLabel)
                contentH += item.lineGap; // gap between label and first value line
            contentH += valLines * textHeightCells + (valLines - 1.0f) * item.lineGap;

            // Width: widest of label or value lines
            for (const auto &line : item.values)
            {
                float lineW = textRenderer.MeasureWidth(line.prefix + line.text, textScale) / grid.cellSizeX;
                contentW = std::max(contentW, lineW);
            }
        }

        item.box.contentWidth = contentW;
        item.box.contentHeight = contentH;
        item.box.outerWidth = 2.0f * mar + 2.0f * pad + contentW;
        item.box.outerHeight = 2.0f * mar + 2.0f * pad + contentH;
    };

    // Resolve an anchor to a cell coordinate.
    // Gap is applied automatically: screen edges push inward, panel edges push outward.
    auto resolveEdge = [&](const PanelAnchor &anchor) -> float
    {
        // Each panel now owns its own margin (GAP/2), so:
        //   screen edges push inward by GAP/2 — the panel's margin fills the rest.
        //   panel-to-panel edges are raw outer edges — adjacent margins provide the gap.
        const float halfGap = UIGrid::GAP * 0.5f;

        if (!anchor.panel)
        {
            switch (anchor.edge)
            {
            case PanelAnchor::Right:
                return static_cast<float>(grid.columns) - halfGap;
            case PanelAnchor::Bottom:
                return static_cast<float>(grid.rows) - halfGap;
            default: // Left, Top
                return halfGap;
            }
        }

        const Panel &other = *anchor.panel;
        switch (anchor.edge)
        {
        case PanelAnchor::Left:
            return other.col;
        case PanelAnchor::Right:
            return other.col + other.colSpan;
        case PanelAnchor::Top:
            return other.row;
        case PanelAnchor::Bottom:
            return other.row + other.rowSpan;
        }
        return 0.0f;
    };

    // Helper to update localGrid from cell coordinates
    auto updateLocalGrid = [&](Panel &item, float padOverride)
    {
        float mx = item.margin;
        float px = grid.ToPixelsX(item.col + mx);
        float py = grid.ToPixelsY(item.row + mx);
        float pw = grid.ToPixelsX(item.colSpan - 2.0f * mx);
        float ph = grid.ToPixelsY(item.rowSpan - 2.0f * mx);
        item.localGrid.Update(px, py, pw, ph, grid.cellSizeX, padOverride);
    };

    // Recursively place children within a parent (vertical stacking).
    // Children are inset horizontally and stacked downward (additive margins).
    auto placeChildrenVertical = [&](this auto &self, Panel &parent) -> void
    {
        if (parent.sections.empty())
            return;

        // Horizontal inset: margin + padding for all parent kinds (symmetric with vertical cursor)
        float insetX = parent.margin + parent.padding;

        // Vertical cursor: skip margin + padding + optional label (childMar above and below label)
        float cursor = parent.row + parent.margin + parent.padding;
        if (parent.showLabel)
        {
            float childMar = Panel::MarginForLayer(parent.layer);
            cursor += childMar + textHeightCells + childMar;
        }

        for (auto &child : parent.sections)
        {
            if (!child.visible)
                continue;

            child.col = parent.col + insetX;
            child.colSpan = parent.colSpan - 2.0f * insetX;

            // Non-stretching children use their content width, centered
            if (!child.stretch && child.box.outerWidth < child.colSpan)
            {
                float availW = child.colSpan;
                child.colSpan = child.box.outerWidth;
                child.col += (availW - child.colSpan) * 0.5f;
            }

            child.row = cursor;
            child.rowSpan = child.box.outerHeight;
            updateLocalGrid(child, child.padding);

            // Recurse into grandchildren
            self(child);

            cursor += child.box.outerHeight;
        }
    };

    // Single linear pass — panels can only reference panels added before them
    for (auto &panel : panels)
    {
        // Compute boxes for this panel and all descendants
        computeBox(panel);

        // --- Horizontal axis ---
        std::optional<float> left, right, w;
        if (panel.leftAnchor)
            left = resolveEdge(*panel.leftAnchor);
        if (panel.rightAnchor)
            right = resolveEdge(*panel.rightAnchor);
        w = panel.width;

        // Auto-width: use box model
        if (!w && !right)
        {
            w = panel.box.outerWidth;

            // Vertical sections: widen panel to fit widest section
            bool willBeVertical = !(panel.leftAnchor.has_value() && panel.rightAnchor.has_value());
            if (willBeVertical)
            {
                for (const auto &section : panel.sections)
                {
                    if (!section.visible)
                        continue;
                    // Section outer width already includes its own margin+padding.
                    // In a vertical panel, sections are inset by parent padding,
                    // so the panel must be wide enough for section + parent padding.
                    float secW = section.box.outerWidth + 2.0f * panel.padding;
                    w = std::max(*w, secW);
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

        // Auto-height: use box model
        if (!h && !bottom)
        {
            h = panel.box.outerHeight;
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

        updateLocalGrid(panel, panel.padding);

        // --- Resolve child section positions ---
        if (!panel.sections.empty())
        {
            bool horizontal = panel.leftAnchor.has_value() && panel.rightAnchor.has_value();

            if (horizontal)
            {
                float secPadding = Panel::PaddingForLayer(1);

                // First child is always the title paragraph; place it at panel origin.
                Panel &titlePara = panel.sections[0];
                titlePara.col = panel.col;
                titlePara.colSpan = titlePara.box.outerWidth;
                titlePara.row = panel.row;
                titlePara.rowSpan = panel.rowSpan;
                updateLocalGrid(titlePara, panel.padding);

                // Remaining children are tab sections stacked rightward
                float headerWidth = titlePara.box.outerWidth;
                float totalWidth = headerWidth;
                for (size_t i = 1; i < panel.sections.size(); i++)
                {
                    const auto &section = panel.sections[i];
                    if (!section.visible)
                        continue;
                    float secTextW = textRenderer.MeasureWidth(section.id, textScale);
                    float secW = 2.0f * secPadding + secTextW / grid.cellSizeX;
                    totalWidth += (section.showSplitter ? SPLITTER_TOTAL : 0.0f) + secW;
                }

                if (!panel.rightAnchor && !panel.width)
                    panel.colSpan = std::max(panel.colSpan, totalWidth);

                float currentCol = panel.col + headerWidth;
                for (size_t i = 1; i < panel.sections.size(); i++)
                {
                    auto &section = panel.sections[i];
                    if (!section.visible)
                        continue;
                    currentCol += section.showSplitter ? SPLITTER_TOTAL : 0.0f;
                    float secTextW = textRenderer.MeasureWidth(section.id, textScale);
                    float secW = 2.0f * secPadding + secTextW / grid.cellSizeX;

                    section.col = currentCol;
                    section.colSpan = secW;
                    section.row = panel.row;
                    section.rowSpan = panel.rowSpan;
                    updateLocalGrid(section, panel.padding);

                    currentCol += secW;
                }
            }
            else
            {
                placeChildrenVertical(panel);
            }

            // Recompute parent local grid after potential size expansion
            updateLocalGrid(panel, panel.padding);
        }
    }
}

void UIRenderer::ComputeMinGridSize()
{
    // Uses precomputed box models from the last ResolveAnchors() pass.
    // Simulates anchor resolution with minimum dimensions to find
    // the smallest screen extent that fits all panels.
    const float gap = UIGrid::GAP;
    float maxRight = 0.0f;
    float maxBottom = 0.0f;

    struct Resolved
    {
        float col, row, colSpan, rowSpan;
    };
    std::vector<Resolved> resolved;
    resolved.reserve(panels.size());

    auto resolveEdgeMin = [&](const PanelAnchor &anchor, float minExtentX, float minExtentY) -> float
    {
        const float halfGap = gap * 0.5f;

        if (!anchor.panel)
        {
            switch (anchor.edge)
            {
            case PanelAnchor::Right:
                return minExtentX - halfGap;
            case PanelAnchor::Bottom:
                return minExtentY - halfGap;
            default:
                return halfGap;
            }
        }

        for (size_t i = 0; i < panels.size(); i++)
        {
            if (&panels[i] == anchor.panel)
            {
                const auto &r = resolved[i];
                switch (anchor.edge)
                {
                case PanelAnchor::Left:
                    return r.col;
                case PanelAnchor::Right:
                    return r.col + r.colSpan;
                case PanelAnchor::Top:
                    return r.row;
                case PanelAnchor::Bottom:
                    return r.row + r.rowSpan;
                }
            }
        }
        return 0.0f;
    };

    float extentX = 1000.0f;
    float extentY = 1000.0f;

    TextMetrics tm = ComputeTextMetrics();
    float textScale = tm.textScale;

    for (const auto &panel : panels)
    {
        Resolved r{};

        // Horizontal
        std::optional<float> left, right;
        if (panel.leftAnchor)
            left = resolveEdgeMin(*panel.leftAnchor, extentX, extentY);
        if (panel.rightAnchor)
            right = resolveEdgeMin(*panel.rightAnchor, extentX, extentY);

        float w = 0.0f;
        if (panel.leftAnchor && panel.rightAnchor)
            w = panel.minWidth.value_or(panel.width.value_or(0.0f));
        else if (panel.width)
            w = *panel.width;
        else
            w = panel.box.outerWidth;

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
            h = panel.box.outerHeight;

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

        // Expand for sections using box model
        if (!panel.sections.empty())
        {
            bool isHorizontal = panel.leftAnchor.has_value() && panel.rightAnchor.has_value();

            if (isHorizontal)
            {
                float totalWidth = !panel.sections.empty() ? panel.sections[0].box.outerWidth : r.colSpan;
                for (size_t i = 1; i < panel.sections.size(); i++)
                {
                    const auto &sec = panel.sections[i];
                    if (!sec.visible)
                        continue;
                    float secTextW = textRenderer.MeasureWidth(sec.id, textScale);
                    float secW = 2.0f * sec.padding + secTextW / grid.cellSizeX;
                    totalWidth += (sec.showSplitter ? SPLITTER_TOTAL : 0.0f) + secW;
                }
                if (!panel.rightAnchor && !panel.width)
                    r.colSpan = totalWidth;
            }
            else
            {
                // Trust box model — panel.box.outerHeight already computed
                if (!panel.bottomAnchor && !panel.height)
                    r.rowSpan = panel.box.outerHeight;
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

    // Emit background for any item (inset by margin from outer box)
    auto emitBackground = [&](const Panel &item)
    {
        float mx = item.margin;
        float sx0 = grid.ToPixelsX(item.col) + grid.ToPixelsX(mx);
        float sy0 = grid.ToPixelsY(item.row) + grid.ToPixelsY(mx);
        float sx1 = grid.ToPixelsX(item.col + item.colSpan) - grid.ToPixelsX(mx);
        float sy1 = grid.ToPixelsY(item.row + item.rowSpan) - grid.ToPixelsY(mx);
        float sr = std::min(grid.cellSizeX, grid.cellSizeY) * item.borderRadius;
        EmitRoundedRect(vertices, indices, vertexOffset, sx0, sy0, sx1, sy1, sr, item.color);
    };

    // Emit a horizontal splitter centered between gapTop and gapBottom (in cells).
    // X extents span parentBgX0..parentBgX1 inset by half the parent's padding.
    auto emitVerticalSplitter = [&](float gapTop, float gapBottom,
                                    float parentBgX0, float parentBgX1,
                                    float parentPadding, glm::vec4 color)
    {
        float halfPadPx = grid.ToPixelsX(parentPadding) * 0.5f;
        float halfSpl = SPLITTER_HEIGHT * 0.5f;
        float splCenterY = (gapTop + gapBottom) * 0.5f;
        float rsx0 = std::round(parentBgX0 + halfPadPx);
        float rsy0 = std::round(grid.ToPixelsY(splCenterY - halfSpl));
        float rsx1 = std::round(parentBgX1 - halfPadPx);
        float rsy1 = std::round(grid.ToPixelsY(splCenterY + halfSpl));
        float sr = std::min(rsx1 - rsx0, rsy1 - rsy0) * 0.5f;
        EmitRoundedRect(vertices, indices, vertexOffset, rsx0, rsy0, rsx1, rsy1, sr, color);
    };

    TextMetrics tm = ComputeTextMetrics();

    // Recursive: emit background, splitters, then recurse into children
    auto emitChildren = [&](this auto &self, const Panel &parent, int colorLevel) -> void
    {
        // Background bounds of this parent (inset by margin)
        float mx = parent.margin;
        float bgX0 = grid.ToPixelsX(parent.col) + grid.ToPixelsX(mx);
        float bgX1 = grid.ToPixelsX(parent.col + parent.colSpan) - grid.ToPixelsX(mx);

        // Track the visual bottom of the previous element for centering splitters.
        // For the header, this is where text ends.
        // For children, this is the background/content bottom (excluding trailing margin).
        float prevVisualBottom = parent.row + parent.margin + parent.padding;
        if (parent.showLabel)
        {
            float childMar = Panel::MarginForLayer(parent.layer);
            prevVisualBottom += childMar + tm.textHeightCells + childMar;
        }

        int visibleIdx = 0;
        for (const auto &child : parent.sections)
        {
            if (!child.visible)
                continue;

            // Splitter logic: Panel draws splitters between children (not before the first).
            // Sections/paragraphs respect their own showSplitter flag.
            bool needsSplitter = false;
            if (parent.kind == UIKind::Panel && visibleIdx > 0)
                needsSplitter = true;
            else if (child.showSplitter && visibleIdx > 0)
                needsSplitter = true;
            else if (parent.showLabel && visibleIdx == 0)
                needsSplitter = true; // header separator for labeled containers

            // Center splitter between visual bottom of prev and visual top of this child
            float childVisualTop = child.row + child.margin;
            if (needsSplitter)
                emitVerticalSplitter(prevVisualBottom, childVisualTop, bgX0, bgX1, parent.padding, Color::GetUI(colorLevel));

            if (child.HasBackground())
                emitBackground(child);

            // Recurse
            self(child, colorLevel + 1);

            prevVisualBottom = child.row + child.box.outerHeight - child.margin;
            visibleIdx++;
        }
    };

    for (const auto &panel : panels)
    {
        if (!panel.visible)
            continue;

        // Panel background
        if (panel.HasBackground())
        {
            float mx = panel.margin;
            float x0 = grid.ToPixelsX(panel.col + mx);
            float y0 = grid.ToPixelsY(panel.row + mx);
            float x1 = grid.ToPixelsX(panel.col + panel.colSpan - mx);
            float y1 = grid.ToPixelsY(panel.row + panel.rowSpan - mx);
            float r = std::min(grid.cellSizeX, grid.cellSizeY) * panel.borderRadius;
            EmitRoundedRect(vertices, indices, vertexOffset, x0, y0, x1, y1, r, panel.color);
        }

        // Horizontal panel: splitters are vertical dividers between sections
        bool horizontal = panel.leftAnchor.has_value() && panel.rightAnchor.has_value();
        if (horizontal)
        {
            float y0 = grid.ToPixelsY(panel.row);
            float y1 = grid.ToPixelsY(panel.row + panel.rowSpan);
            float halfPadPx = grid.ToPixelsX(panel.padding) * 0.5f;
            for (size_t si = 1; si < panel.sections.size(); si++)
            {
                const auto &section = panel.sections[si];
                if (!section.visible)
                    continue;
                float halfSpl = SPLITTER_HEIGHT * 0.5f;
                float rsx0 = std::round(grid.ToPixelsX(section.col - halfSpl));
                float rsx1 = std::round(grid.ToPixelsX(section.col + halfSpl));
                float rsy0 = std::round(y0 + halfPadPx);
                float rsy1 = std::round(y1 - halfPadPx);
                float sr = std::min(rsx1 - rsx0, rsy1 - rsy0) * 0.5f;
                EmitRoundedRect(vertices, indices, vertexOffset, rsx0, rsy0, rsx1, rsy1, sr, Color::GetUI(2));
            }

        }
        else
        {
            emitChildren(panel, 2);
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

    // Recursive helper to render text/ImGui content for an item and its children
    auto renderItem = [&](this auto &self, const Panel &item, const std::string &parentPath) -> void
    {
        if (!item.visible)
            return;

        std::string itemPath = parentPath + "_" + item.id;

        const auto &slg = item.localGrid;
        float sTextScale = slg.cellSizeY / textRenderer.GetLineHeight(1.0f) * 1.4f;
        float bearingY = textRenderer.GetMaxBearingY(sTextScale);
        float lineStepPx = item.padding * Panel::LINE_GAP_RATIO * grid.cellSizeY + bearingY;

        // Content origin: margin + padding inset from outer box
        float contentPx = grid.ToPixelsX(item.col + item.margin + item.padding);
        float contentPy = grid.ToPixelsY(item.row + item.margin + item.padding);

        if (item.HasBackground())
        {
            // Background edge = margin inset from outer box
            float mx = item.margin;
            float bgX0 = grid.ToPixelsX(item.col + mx);
            float bgY0 = grid.ToPixelsY(item.row + mx);
            float bgX1 = grid.ToPixelsX(item.col + item.colSpan - mx);
            float bgY1 = grid.ToPixelsY(item.row + item.rowSpan - mx);

            if (!item.id.empty() && item.showLabel)
            {
                if (!item.sections.empty())
                {
                    // Sub-panel style: top-left header inset by childMar
                    float childMar = Panel::MarginForLayer(item.layer) * grid.cellSizeY;
                    float labelPx = contentPx + item.labelLeftInset * grid.cellSizeX;
                    float labelPy = contentPy + childMar;
                    textRenderer.RenderText(item.id, labelPx, labelPy + bearingY, sTextScale, Color::GetUIText(1));
                }
                else
                {
                    // Button style: centered in background
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
                float childMar = Panel::MarginForLayer(item.layer) * grid.cellSizeY;
                textRenderer.RenderText(item.id, contentPx, contentPy + childMar + bearingY, sTextScale, Color::GetUIText(1));
            }

            float valueStart = item.showLabel ? 1.0f : 0.0f;
            for (size_t i = 0; i < item.values.size(); i++)
            {
                const auto &line = item.values[i];
                float vpx = contentPx;
                float vpy = contentPy + bearingY + lineStepPx * (valueStart + static_cast<float>(i));

                float btnX = vpx;
                float btnY = vpy - bearingY;
                float btnW = (slg.columns - 2.0f * slg.padding) * slg.cellSizeX;
                float btnH = bearingY;
                ImGui::SetNextWindowPos(ImVec2(btnX, btnY));
                ImGui::SetNextWindowSize(ImVec2(btnW, btnH));
                std::string winId = "##val" + itemPath + "_" + std::to_string(i);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
                if (ImGui::Begin(winId.c_str(), nullptr,
                                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings))
                {
                    if (line.imguiContent)
                    {
                        if (pixelImFont)
                            ImGui::PushFont(pixelImFont);
                        line.imguiContent(btnW, btnH);
                        if (pixelImFont)
                            ImGui::PopFont();
                    }
                    else
                    {
                        if (line.onClick)
                        {
                            ImGui::SetCursorPos(ImVec2(0, 0));
                            if (ImGui::InvisibleButton(("##btn" + std::to_string(i)).c_str(), ImVec2(btnW, btnH)))
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
                            if (line.onClick)
                            {
                                float tw = ImGui::CalcTextSize(line.text.c_str()).x;
                                float underlineY = ty + ImGui::GetFontSize();
                                dl->AddLine(ImVec2(tx, underlineY), ImVec2(tx + tw, underlineY), textCol, 1.0f);
                            }
                        }
                    }
                }
                ImGui::End();
                ImGui::PopStyleVar(2);
            }
        }

        // Recurse into children
        for (const auto &child : item.sections)
            self(child, itemPath);
    };

    // Render all panels and their descendants recursively
    for (const auto &panel : panels)
    {
        if (!panel.visible)
            continue;
        renderItem(panel, "");
    }

    // --- Debug layout overlay ---
    if (debugLayout)
    {
        ImDrawList *dl = ImGui::GetForegroundDrawList();

        auto drawDebugItem = [&](this auto &self, const Panel &item) -> void
        {
            if (!item.visible)
                return;

            float ox0 = grid.ToPixelsX(item.col);
            float oy0 = grid.ToPixelsY(item.row);
            float ox1 = grid.ToPixelsX(item.col + item.colSpan);
            float oy1 = grid.ToPixelsY(item.row + item.rowSpan);

            float bx0 = grid.ToPixelsX(item.col + item.margin);
            float by0 = grid.ToPixelsY(item.row + item.margin);
            float bx1 = grid.ToPixelsX(item.col + item.colSpan - item.margin);
            float by1 = grid.ToPixelsY(item.row + item.rowSpan - item.margin);

            float cx0 = grid.ToPixelsX(item.col + item.margin + item.padding);
            float cy0 = grid.ToPixelsY(item.row + item.margin + item.padding);
            float cx1 = grid.ToPixelsX(item.col + item.colSpan - item.margin - item.padding);
            float cy1 = grid.ToPixelsY(item.row + item.rowSpan - item.margin - item.padding);

            // Fill only the zone bands as 4-strip donuts — content area stays clear.
            // Blue  = margin zone  (outer       → background boundary)
            // Green = padding zone (background  → content boundary)
            auto fillRing = [&](float x0, float y0, float x1, float y1,
                                float ix0, float iy0, float ix1, float iy1,
                                ImU32 col)
            {
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, iy0), col);   // top
                dl->AddRectFilled(ImVec2(x0, iy1), ImVec2(x1, y1), col);   // bottom
                dl->AddRectFilled(ImVec2(x0, iy0), ImVec2(ix0, iy1), col); // left
                dl->AddRectFilled(ImVec2(ix1, iy0), ImVec2(x1, iy1), col); // right
            };
            fillRing(ox0, oy0, ox1, oy1, cx0, cy0, cx1, cy1, IM_COL32(0, 140, 255, 70));

            // Outlines
            dl->AddRect(ImVec2(ox0, oy0), ImVec2(ox1, oy1), IM_COL32(0, 140, 255, 200), 0.0f, 0, 1.0f);
            dl->AddRect(ImVec2(cx0, cy0), ImVec2(cx1, cy1), IM_COL32(0, 210, 90, 200), 0.0f, 0, 1.0f);

            for (const auto &child : item.sections)
                self(child);
        };

        for (const auto &panel : panels)
            drawDebugItem(panel);
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
        if (!panel.visible || !panel.HasBackground())
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
