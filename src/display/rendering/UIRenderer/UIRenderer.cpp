#include "UIRenderer.hpp"
#include "rendering/color.hpp"
#include "UIStyle.hpp"
#include "utils/log.hpp"
#include "imgui.h"
#include <algorithm>
#include <cmath>

static constexpr float SPLITTER_HEIGHT = 0.125f; // splitter line thickness in cells
static constexpr float SPLITTER_PAD = 0.125f;    // padding above and below the splitter line
static constexpr float SPLITTER_TOTAL = SPLITTER_HEIGHT + 2.0f * SPLITTER_PAD;

// Returns the pixel bounding box for text as ImGui's AddText would produce, plus optionally
// the underline drawn by onClick lines. Origin = (x,y) passed to AddText; results relative to (0,0).
// Underline is drawn 1px below the font baseline (font->Ascent). Center snapped to floor(Ascent+1)+0.5,
// thickness 1 → worst-case bottom = Ascent + 2.0 (relative to the AddText origin).
static PixelBounds MeasureImGuiInkBounds(const std::string &text, ImFont *font, bool includeUnderline = false)
{
    PixelBounds b;
    if (!font || text.empty())
        return b;
    float cx = 0.0f;
    for (unsigned char c : text)
    {
        const ImFontGlyph *g = font->FindGlyph(static_cast<ImWchar>(c));
        if (!g)
            continue;
        b.expand(cx + g->X0, g->Y0);
        b.expand(cx + g->X1, g->Y1);
        cx += g->AdvanceX;
    }
    if (includeUnderline && b.valid())
        b.expand(b.x0, std::max(b.y1, font->Ascent + 2.0f)); // underline at baseline+1px; worst-case bottom = Ascent+2
    return b;
}

TextMetrics UIRenderer::ComputeTextMetrics() const
{
    TextMetrics tm;
    tm.localCell = grid.cellSizeX * PanelGrid::LOCAL_CELL_RATIO;
    tm.textScale = tm.localCell / textRenderer.GetLineHeight(1.0f) * 1.4f;
    tm.textHeightCells = textRenderer.GetMaxBearingY(tm.textScale) / grid.cellSizeY;
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

RootPanel &UIRenderer::AddPanel(const RootPanel &panel)
{
    panels.push_back(panel);
    dirty = true;
    return panels.back();
}

RootPanel *UIRenderer::GetPanel(const std::string &id)
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
    RootPanel *panel = GetPanel(panelId);
    if (!panel)
        return;

    auto searchSection = [&](Section &sec) -> bool
    {
        if (sec.id == sectionId)
        {
            assert(false && "SetSectionValue: target ID matches a Section; Sections hold no values directly");
            return false;
        }
        for (auto &child : sec.children)
        {
            if (child.id == sectionId)
            {
                child.values = values;
                dirty = true;
                return true;
            }
        }
        return false;
    };
    for (auto &child : panel->children)
    {
        if (std::holds_alternative<Paragraph>(child))
        {
            auto &p = std::get<Paragraph>(child);
            if (p.id == sectionId)
            {
                p.values = values;
                dirty = true;
                return;
            }
        }
        else
        {
            if (searchSection(std::get<Section>(child)))
                return;
        }
    }
}

void UIRenderer::SetSectionVisible(const std::string &panelId, const std::string &sectionId, bool visible)
{
    RootPanel *panel = GetPanel(panelId);
    if (!panel)
        return;

    auto setVisible = [&](UIElement &el) -> bool
    {
        if (el.id == sectionId)
        {
            if (el.visible != visible)
            {
                el.visible = visible;
                dirty = true;
            }
            return true;
        }
        return false;
    };
    for (auto &child : panel->children)
    {
        bool found = std::visit([&](auto &el) -> bool
                                {
            if (setVisible(el))
                return true;
            if constexpr (std::is_same_v<std::decay_t<decltype(el)>, Section>)
            {
                for (auto &p : el.children)
                    if (setVisible(p))
                        return true;
            }
            return false; }, child);
        if (found)
            return;
    }
}

void UIRenderer::ResolveAnchors()
{
    // --- Text metrics (computed once, reused throughout) ---
    TextMetrics tm = ComputeTextMetrics();
    float textScale = tm.textScale;
    float textHeightCells = tm.textHeightCells;

    // --- Compute content boxes bottom-up ---
    // Each element's box is: margin + padding + content + padding + margin (additive, no collapsing).

    // Compute box for a Paragraph leaf.
    auto computeParagraphBox = [&](Paragraph &item) -> void
    {
        float pad = item.padding;
        float mar = item.margin;
        item.lineGap = pad * UIElement::LINE_GAP_RATIO;

        float contentH = 0.0f;
        float contentW = 0.0f;

        if (!item.values.empty())
        {
            float bearingPx = grid.cellSizeX * PanelGrid::LOCAL_CELL_RATIO;
            float lineGapPx = item.lineGap * grid.cellSizeY;
            // imguiContent lines (e.g. DragFloat) need at least GetFrameHeight() of vertical space.
            float slotH = bearingPx;
            if (pixelImFont)
            {
                for (const auto &v : item.values)
                    if (v.imguiContent)
                    {
                        slotH = std::max(slotH, pixelImFont->FontSize + ImGui::GetStyle().FramePadding.y * 2.0f);
                        break;
                    }
            }
            float lineStepPx = lineGapPx + slotH;

            float maxBottom = 0.0f;
            for (size_t i = 0; i < item.values.size(); i++)
            {
                const auto &line = item.values[i];
                ImFont *lineFont = (line.onClick && pixelImFont) ? pixelImFont : cachedTextImFont;
                PixelBounds ink = MeasureImGuiInkBounds(line.prefix + line.text, lineFont, bool(line.onClick));
                float inkH = line.imguiContent ? slotH
                                               : (ink.valid() ? std::max(ink.height(), bearingPx) : bearingPx);
                // ink.x1 is the visual right edge of the last glyph (includes overhang past AdvanceX)
                float inkW = ink.valid() ? ink.x1 : textRenderer.MeasureWidth(line.prefix + line.text, textScale);
                maxBottom = std::max(maxBottom, lineStepPx * static_cast<float>(i) + inkH);
                contentW = std::max(contentW, inkW / grid.cellSizeX);
            }
            contentH += maxBottom / grid.cellSizeY;
        }

        item.box.contentWidth = contentW;
        item.box.contentHeight = contentH;
        item.box.outerWidth = 2.0f * mar + 2.0f * pad + contentW;
        item.box.outerHeight = 2.0f * mar + 2.0f * pad + contentH;
    };

    // Compute box for a Section (label + Paragraph children).
    auto computeSectionBox = [&](Section &item) -> void
    {
        float pad = item.padding;
        float mar = item.margin;

        for (auto &child : item.children)
            computeParagraphBox(child);

        float contentH = 0.0f;
        float contentW = 0.0f;

        if (item.header.has_value())
        {
            computeParagraphBox(item.header->para);
            contentH = item.header->para.box.outerHeight;
            contentW = item.header->para.box.outerWidth;
        }

        for (auto &child : item.children)
        {
            if (!child.visible)
                continue;
            contentH += child.box.outerHeight;
            contentW = std::max(contentW, child.box.outerWidth);
        }

        item.box.contentWidth = contentW;
        item.box.contentHeight = contentH;
        item.box.outerWidth = 2.0f * mar + 2.0f * pad + contentW;
        item.box.outerHeight = 2.0f * mar + 2.0f * pad + contentH;
    };

    // Compute box for a RootPanel — dispatches into Section/Paragraph children.
    auto computeBox = [&](RootPanel &item) -> void
    {
        float pad = item.padding;
        float mar = item.margin;

        for (auto &child : item.children)
            std::visit([&](auto &el)
                       { if constexpr (std::is_same_v<std::decay_t<decltype(el)>, Section>) computeSectionBox(el); else computeParagraphBox(el); }, child);

        float contentH = 0.0f;
        float contentW = 0.0f;

        // Optional label on root panel (anonymous containers have no header)
        if (item.header.has_value())
        {
            computeParagraphBox(item.header->para);
            contentH = item.header->para.box.outerHeight;
            contentW = item.header->para.box.outerWidth;
        }

        for (auto &child : item.children)
        {
            const UIElement &el = std::visit([](auto &e) -> const UIElement &
                                             { return e; }, child);
            if (!el.visible)
                continue;
            contentH += el.box.outerHeight;
            contentW = std::max(contentW, el.box.outerWidth);
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

        const RootPanel &other = *anchor.panel;
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
    auto updateLocalGrid = [&](UIElement &item, float padOverride)
    {
        float mx = item.margin;
        float px = grid.ToPixelsX(item.col + mx);
        float py = grid.ToPixelsY(item.row + mx);
        float pw = grid.ToPixelsX(item.colSpan - 2.0f * mx);
        float ph = grid.ToPixelsY(item.rowSpan - 2.0f * mx);
        item.localGrid.Update(px, py, pw, ph, grid.cellSizeX, padOverride);
    };

    // Place Paragraph children within a Section (vertical stacking).
    auto placeSectionChildren = [&](Section &parent) -> void
    {
        float insetX = parent.margin + parent.padding;
        float cursor = parent.row + parent.margin + parent.padding;
        if (parent.header.has_value())
        {
            Paragraph &hpara = parent.header->para;
            hpara.col = parent.col + insetX;
            hpara.colSpan = parent.colSpan - 2.0f * insetX;
            hpara.row = cursor;
            hpara.rowSpan = hpara.box.outerHeight;
            updateLocalGrid(hpara, hpara.padding);
            cursor += hpara.box.outerHeight;
        }

        for (auto &child : parent.children)
        {
            if (!child.visible)
                continue;

            child.col = parent.col + insetX;
            child.colSpan = parent.colSpan - 2.0f * insetX;

            if (!child.stretch && child.box.outerWidth < child.colSpan)
            {
                float availW = child.colSpan;
                child.colSpan = child.box.outerWidth;
                child.col += (availW - child.colSpan) * 0.5f;
            }

            child.row = cursor;
            child.rowSpan = child.box.outerHeight;
            updateLocalGrid(child, child.padding);

            cursor += child.box.outerHeight;
        }
    };

    // Place children of a RootPanel (vertical stacking).
    auto placeChildrenVertical = [&](RootPanel &parent) -> void
    {
        if (parent.children.empty())
            return;

        float insetX = parent.margin + parent.padding;
        float cursor = parent.row + parent.margin + parent.padding;
        if (parent.header.has_value())
        {
            Paragraph &hpara = parent.header->para;
            hpara.col = parent.col + insetX;
            hpara.colSpan = parent.colSpan - 2.0f * insetX;
            hpara.row = cursor;
            hpara.rowSpan = hpara.box.outerHeight;
            updateLocalGrid(hpara, hpara.padding);
            cursor += hpara.box.outerHeight;
        }

        for (auto &child : parent.children)
        {
            UIElement &el = std::visit([](auto &e) -> UIElement &
                                       { return e; }, child);
            if (!el.visible)
                continue;

            el.col = parent.col + insetX;
            el.colSpan = parent.colSpan - 2.0f * insetX;

            if (!el.stretch && el.box.outerWidth < el.colSpan)
            {
                float availW = el.colSpan;
                el.colSpan = el.box.outerWidth;
                el.col += (availW - el.colSpan) * 0.5f;
            }

            el.row = cursor;
            el.rowSpan = el.box.outerHeight;
            updateLocalGrid(el, el.padding);

            if (std::holds_alternative<Section>(child))
                placeSectionChildren(std::get<Section>(child));

            cursor += el.box.outerHeight;
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
        // (computeBox already accounts for the widest child, so panel.box.outerWidth suffices)
        if (!w && !right)
        {
            w = panel.box.outerWidth;
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

        // --- Resolve child positions ---
        if (!panel.children.empty())
        {
            bool horizontal = panel.leftAnchor.has_value() && panel.rightAnchor.has_value();

            if (horizontal)
            {
                float secPadding = UIElement::PaddingForLayer(1);

                // First child is always the title paragraph; place it inside the panel's content area.
                UIElement &titleEl = std::visit([](auto &e) -> UIElement &
                                                { return e; }, panel.children[0]);
                float panelInset = panel.margin + panel.padding;
                titleEl.col = panel.col + panelInset;
                titleEl.colSpan = titleEl.box.outerWidth;
                titleEl.row = panel.row + panelInset;
                titleEl.rowSpan = panel.rowSpan - 2.0f * panelInset;
                updateLocalGrid(titleEl, titleEl.padding);

                // Remaining children are tab sections stacked rightward (splitter between each)
                float headerWidth = panelInset + titleEl.box.outerWidth;
                float totalWidth = headerWidth;
                for (size_t i = 1; i < panel.children.size(); i++)
                {
                    const UIElement &el = std::visit([](const auto &e) -> const UIElement &
                                                     { return e; }, panel.children[i]);
                    if (!el.visible)
                        continue;
                    float secTextW = textRenderer.MeasureWidth(el.id, textScale);
                    float secW = 2.0f * secPadding + secTextW / grid.cellSizeX;
                    totalWidth += SPLITTER_TOTAL + secW;
                }

                if (!panel.rightAnchor && !panel.width)
                    panel.colSpan = std::max(panel.colSpan, totalWidth + panelInset);

                float currentCol = panel.col + headerWidth;
                for (size_t i = 1; i < panel.children.size(); i++)
                {
                    UIElement &el = std::visit([](auto &e) -> UIElement &
                                               { return e; }, panel.children[i]);
                    if (!el.visible)
                        continue;
                    currentCol += SPLITTER_TOTAL;
                    float secTextW = textRenderer.MeasureWidth(el.id, textScale);
                    float secW = 2.0f * secPadding + secTextW / grid.cellSizeX;

                    el.col = currentCol;
                    el.colSpan = secW;
                    el.row = panel.row;
                    el.rowSpan = panel.rowSpan;
                    updateLocalGrid(el, panel.padding);

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
    // Ensure box models are up to date before simulating layout.
    ResolveAnchors();

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

        // Expand for children using box model
        if (!panel.children.empty())
        {
            bool isHorizontal = panel.leftAnchor.has_value() && panel.rightAnchor.has_value();

            if (isHorizontal)
            {
                const UIElement &first = std::visit([](const auto &e) -> const UIElement &
                                                    { return e; }, panel.children[0]);
                float totalWidth = first.box.outerWidth;
                for (size_t i = 1; i < panel.children.size(); i++)
                {
                    const UIElement &el = std::visit([](const auto &e) -> const UIElement &
                                                     { return e; }, panel.children[i]);
                    if (!el.visible)
                        continue;
                    float secTextW = textRenderer.MeasureWidth(el.id, textScale);
                    float secW = 2.0f * el.padding + secTextW / grid.cellSizeX;
                    totalWidth += SPLITTER_TOTAL + secW;
                }
                if (!panel.rightAnchor && !panel.width)
                    r.colSpan = totalWidth;
            }
            else
            {
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

    TextMetrics tm = ComputeTextMetrics();

    // Emit background rect for a UIElement.
    auto emitBackground = [&](const UIElement &item, glm::vec4 color)
    {
        float mx = item.margin;
        float sx0 = grid.ToPixelsX(item.col) + grid.ToPixelsX(mx);
        float sy0 = grid.ToPixelsY(item.row) + grid.ToPixelsY(mx);
        float sx1 = grid.ToPixelsX(item.col + item.colSpan) - grid.ToPixelsX(mx);
        float sy1 = grid.ToPixelsY(item.row + item.rowSpan) - grid.ToPixelsY(mx);
        float sr = std::min(grid.cellSizeX, grid.cellSizeY) * item.borderRadius;
        EmitRoundedRect(vertices, indices, vertexOffset, sx0, sy0, sx1, sy1, sr, color);
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

    // Emit children of a RootPanel: splitters between each visible child, then recurse.
    // Splitter policy: always between children (parent draws them, not children).
    auto emitPanelChildren = [&](const RootPanel &parent, int colorLevel) -> void
    {
        float mx = parent.margin;
        float bgX0 = grid.ToPixelsX(parent.col) + grid.ToPixelsX(mx);
        float bgX1 = grid.ToPixelsX(parent.col + parent.colSpan) - grid.ToPixelsX(mx);

        float prevVisualBottom = parent.row + parent.margin + parent.padding;
        if (parent.header.has_value())
            prevVisualBottom = parent.header->para.row + parent.header->para.rowSpan - parent.header->para.margin;

        int visibleIdx = 0;
        for (const auto &child : parent.children)
        {
            const UIElement &el = std::visit([](const auto &e) -> const UIElement &
                                             { return e; }, child);
            if (!el.visible)
                continue;

            // Splitter between every visible child, and after the label header.
            bool needsSplitter = (visibleIdx > 0) || (parent.header.has_value() && visibleIdx == 0);
            // For Sections, the visual boundary is the content edge (margin+padding inset), not the
            // outer margin edge. This places the splitter at the true midpoint of the visible gap.
            float childVisualTop = el.row + el.margin;
            if (std::holds_alternative<Section>(child))
                childVisualTop = el.row + el.margin + el.padding;
            if (needsSplitter)
                emitVerticalSplitter(prevVisualBottom, childVisualTop, bgX0, bgX1, parent.padding, Color::GetUI(colorLevel));

            // Emit Section children (Paragraphs inside a Section)
            if (std::holds_alternative<Section>(child))
            {
                const Section &sec = std::get<Section>(child);
                float secMx = sec.margin;
                float secBgX0 = grid.ToPixelsX(sec.col) + grid.ToPixelsX(secMx);
                float secBgX1 = grid.ToPixelsX(sec.col + sec.colSpan) - grid.ToPixelsX(secMx);
                float secPrevBottom = sec.row + sec.margin + sec.padding;
                if (sec.header.has_value())
                    secPrevBottom = sec.header->para.row + sec.header->para.rowSpan - sec.header->para.margin;

                int secVisibleIdx = 0;
                for (const auto &para : sec.children)
                {
                    if (!para.visible)
                        continue;
                    // No splitter before the first paragraph — the section label acts as the header.
                    if (secVisibleIdx > 0)
                        emitVerticalSplitter(secPrevBottom, para.row + para.margin, secBgX0, secBgX1, parent.padding, Color::GetUI(colorLevel + 1));
                    secPrevBottom = para.row + para.box.outerHeight - para.margin;
                    secVisibleIdx++;
                }
            }

            // For Sections, use the content boundary (margin+padding inset) as the visual bottom edge,
            // matching the same logic used for childVisualTop above.
            if (std::holds_alternative<Section>(child))
                prevVisualBottom = el.row + el.rowSpan - el.margin - el.padding;
            else
                prevVisualBottom = el.row + el.box.outerHeight - el.margin;
            visibleIdx++;
        }
    };

    for (const auto &panel : panels)
    {
        if (!panel.visible)
            continue;

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

        bool horizontal = panel.leftAnchor.has_value() && panel.rightAnchor.has_value();
        if (horizontal)
        {
            float y0 = grid.ToPixelsY(panel.row);
            float y1 = grid.ToPixelsY(panel.row + panel.rowSpan);
            float halfPadPx = grid.ToPixelsX(panel.padding) * 0.5f;
            for (size_t si = 1; si < panel.children.size(); si++)
            {
                const UIElement &el = std::visit([](const auto &e) -> const UIElement &
                                                 { return e; }, panel.children[si]);
                if (!el.visible)
                    continue;
                float halfSpl = SPLITTER_HEIGHT * 0.5f;
                float rsx0 = std::round(grid.ToPixelsX(el.col - halfSpl));
                float rsx1 = std::round(grid.ToPixelsX(el.col + halfSpl));
                float rsy0 = std::round(y0 + halfPadPx);
                float rsy1 = std::round(y1 - halfPadPx);
                float sr = std::min(rsx1 - rsx0, rsy1 - rsy0) * 0.5f;
                EmitRoundedRect(vertices, indices, vertexOffset, rsx0, rsy0, rsx1, rsy1, sr, Color::GetUI(2));
            }
        }
        else
        {
            emitPanelChildren(panel, 2);
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

    cachedTextImFont = ImGui::GetFont();

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

    // Render text for a Paragraph's value lines (interactive ImGui overlay path).
    auto renderParagraph = [&](const Paragraph &item, const std::string &itemPath) -> void
    {
        if (!item.visible)
            return;

        const auto &slg = item.localGrid;
        float bearingY = slg.cellSizeY;
        // imguiContent lines need a slot tall enough for GetFrameHeight().
        float slotH = bearingY;
        if (pixelImFont)
        {
            for (const auto &v : item.values)
                if (v.imguiContent)
                {
                    slotH = std::max(slotH, pixelImFont->FontSize + ImGui::GetStyle().FramePadding.y * 2.0f);
                    break;
                }
        }
        float lineStepPx = item.lineGap * grid.cellSizeY + slotH;
        float contentPx = grid.ToPixelsX(item.col + item.margin + item.padding);
        float contentPy = grid.ToPixelsY(item.row + item.margin + item.padding);

        for (size_t i = 0; i < item.values.size(); i++)
        {
            const auto &line = item.values[i];
            // Measure actual glyph ink bounds at add-text origin (0,0).
            ImFont *font = (line.onClick && pixelImFont) ? pixelImFont : ImGui::GetFont();
            PixelBounds ink = MeasureImGuiInkBounds(line.prefix + line.text, font, false);
            float inkH = ink.valid() ? ink.height() : bearingY;
            float inkY0 = ink.valid() ? ink.y0 : 0.0f;

            float vpx = contentPx;
            float vpy = contentPy + bearingY + lineStepPx * static_cast<float>(i);

            float btnX = vpx;
            float btnY = vpy - bearingY;
            float btnW = (slg.columns - 2.0f * slg.padding) * slg.cellSizeX;
            // Slot height: use frame height for imguiContent, at least text bearing otherwise
            float btnH = line.imguiContent ? slotH : std::max(inkH, bearingY);
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
                    // Centre the ink region in the slot; account for glyph y0 inset
                    float ty = btnY + (btnH - inkH) * 0.5f - inkY0;
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    // Draw framed background for interactive lines (same style as DragFloat sliders)
                    if (line.onClick)
                    {
                        float radius = btnH * UIStyle::FRAME_ROUNDING_RATIO;
                        ImVec4 bgVec;
                        if (ImGui::IsItemActive())
                            bgVec = UIStyle::FrameBgActiveColor();
                        else if (ImGui::IsItemHovered())
                            bgVec = UIStyle::FrameBgHoveredColor();
                        else
                        {
                            glm::vec4 bg = Color::GetInputBg(item.layer);
                            bgVec = ImVec4(bg.r, bg.g, bg.b, bg.a);
                        }
                        dl->AddRectFilled(ImVec2(btnX, btnY), ImVec2(btnX + btnW, btnY + btnH),
                                          ImGui::GetColorU32(bgVec), radius);
                    }
                    float tx = btnX;
                    if (!line.prefix.empty())
                    {
                        ImU32 pc = ImGui::GetColorU32(ImVec4(line.prefixColor.r, line.prefixColor.g, line.prefixColor.b, line.prefixColor.a));
                        dl->AddText(font, font->FontSize, ImVec2(tx, ty), pc, line.prefix.c_str());
                        tx += font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, line.prefix.c_str()).x;
                    }
                    if (!line.text.empty())
                    {
                        glm::vec4 tc = Color::GetUIText(1);
                        ImU32 textCol = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
                        dl->AddText(font, font->FontSize, ImVec2(tx, ty), textCol, line.text.c_str());
                    }
                }
            }
            ImGui::End();
            ImGui::PopStyleVar(2);
        }
    };

    // Render label for a Section header.
    auto renderSection = [&](const Section &sec, const std::string &parentPath) -> void
    {
        if (!sec.visible)
            return;
        std::string secPath = parentPath + "_" + sec.id;

        if (sec.header.has_value())
            renderParagraph(sec.header->para, secPath + "_header");

        for (const auto &para : sec.children)
            renderParagraph(para, secPath + "_" + para.id);
    };

    // Render text for all panels.
    for (const auto &panel : panels)
    {
        if (!panel.visible)
            continue;

        std::string panelPath = "_" + panel.id;

        // Render RootPanel header (anonymous containers have no header)
        if (panel.header.has_value())
            renderParagraph(panel.header->para, panelPath + "_header");
        for (const auto &child : panel.children)
        {
            std::visit([&](const auto &el)
                       {
                using T = std::decay_t<decltype(el)>;
                if constexpr (std::is_same_v<T, Section>)
                    renderSection(el, panelPath);
                else
                    renderParagraph(el, panelPath + "_" + el.id); }, child);
        }
    }

    // --- Debug layout overlay ---
    if (debugLayout)
    {
        ImDrawList *dl = ImGui::GetForegroundDrawList();

        auto drawDebugElement = [&](const UIElement &item)
        {
            if (!item.visible)
                return;

            float ox0 = grid.ToPixelsX(item.col);
            float oy0 = grid.ToPixelsY(item.row);
            float ox1 = grid.ToPixelsX(item.col + item.colSpan);
            float oy1 = grid.ToPixelsY(item.row + item.rowSpan);

            float cx0 = grid.ToPixelsX(item.col + item.margin + item.padding);
            float cy0 = grid.ToPixelsY(item.row + item.margin + item.padding);
            float cx1 = grid.ToPixelsX(item.col + item.colSpan - item.margin - item.padding);
            float cy1 = grid.ToPixelsY(item.row + item.rowSpan - item.margin - item.padding);

            auto fillRing = [&](float x0, float y0, float x1, float y1,
                                float ix0, float iy0, float ix1, float iy1,
                                ImU32 col)
            {
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, iy0), col);
                dl->AddRectFilled(ImVec2(x0, iy1), ImVec2(x1, y1), col);
                dl->AddRectFilled(ImVec2(x0, iy0), ImVec2(ix0, iy1), col);
                dl->AddRectFilled(ImVec2(ix1, iy0), ImVec2(x1, iy1), col);
            };
            fillRing(ox0, oy0, ox1, oy1, cx0, cy0, cx1, cy1, IM_COL32(0, 140, 255, 70));
            dl->AddRect(ImVec2(ox0, oy0), ImVec2(ox1, oy1), IM_COL32(0, 140, 255, 200), 0.0f, 0, 1.0f);
            dl->AddRect(ImVec2(cx0, cy0), ImVec2(cx1, cy1), IM_COL32(0, 210, 90, 200), 0.0f, 0, 1.0f);
        };

        for (const auto &panel : panels)
        {
            drawDebugElement(panel);
            if (panel.header.has_value())
                drawDebugElement(panel.header->para);

            for (const auto &child : panel.children)
            {
                std::visit([&](const auto &el)
                           {
                    drawDebugElement(el);
                    if constexpr (std::is_same_v<std::decay_t<decltype(el)>, Section>)
                    {
                        if (el.header.has_value())
                            drawDebugElement(el.header->para);
                        for (const auto &para : el.children)
                            drawDebugElement(para);
                    } }, child);
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
