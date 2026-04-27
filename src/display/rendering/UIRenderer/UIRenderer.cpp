#include "UIRenderer.hpp"
#include "rendering/color.hpp"
#include "UIStyle.hpp"
#include "Icons.hpp"
#include "utils/log.hpp"
#include "imgui.h"
#include <algorithm>
#include <cmath>

static constexpr float SPLITTER_HEIGHT = 0.125f; // splitter line thickness in cells
static constexpr float SPLITTER_PAD = 0.125f;    // padding above and below the splitter line
static constexpr float SPLITTER_TOTAL = SPLITTER_HEIGHT + 2.0f * SPLITTER_PAD;
static constexpr float ACCENT_SAT_MULT_HOVER = 0.6f; // muted accent strength for hover/active row tint
// Icon slot: s = max(2, round(lineBearing * ICON_SIZE_RATIO)); slot = 2s + 3 px wide
// ICON_SIZE_RATIO and ICON_SIZE_RATIO_SMALL are defined in UIElement (Panel.hpp).
static constexpr float ICON_SIZE_RATIO = UIElement::ICON_SIZE_RATIO;
static constexpr float ICON_SIZE_RATIO_SMALL = UIElement::ICON_SIZE_RATIO_SMALL;
// Returns the pixel bounding box for text as ImGui's AddText would produce.
// Origin = (x,y) passed to AddText; results relative to (0,0).
static PixelBounds MeasureImGuiInkBounds(const std::string &text, ImFont *font)
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
            float bearingPx = pixelImFont ? pixelImFont->FontSize
                                          : grid.cellSizeX * PanelGrid::LOCAL_CELL_RATIO;
            float lineGapPx = item.lineGap * grid.cellSizeY;
            // imguiContent lines (e.g. DragFloat) need at least GetFrameHeight() of vertical space.
            float slotH = bearingPx;
            if (pixelImFont)
            {
                for (const auto &v : item.values)
                    if (v.imguiContent || v.select.has_value())
                    {
                        slotH = std::max(slotH, pixelImFont->FontSize + ImGui::GetStyle().FramePadding.y * 2.0f);
                        break;
                    }
            }
            float lineStepPx = lineGapPx + slotH;

            // Leading slot (e.g. checkbox) advances the text origin in renderParagraph().
            // Account for that here so content width matches what will be drawn.
            float leadingAdvanceCells = 0.0f;
            if (item.leadingDraw && item.leadingWidth > 0.0f)
            {
                constexpr float LEADING_GAP_PX = 4.0f; // must match renderParagraph()
                float leadingPx = grid.ToPixelsX(item.leadingWidth);
                float checkboxW = std::min(slotH, leadingPx);
                leadingAdvanceCells = (checkboxW + LEADING_GAP_PX) / grid.cellSizeX;
            }

            float maxBottom = 0.0f;
            size_t visibleI = 0;
            for (size_t i = 0; i < item.values.size(); i++)
            {
                const auto &line = item.values[i];
                if (!line.visible)
                    continue;
                ImFont *lineFont = (line.onClick && pixelImFont) ? pixelImFont
                                   : (!line.bold && bodyImFont)  ? bodyImFont
                                                                 : cachedTextImFont;
                PixelBounds ink = MeasureImGuiInkBounds(line.prefix + line.text, lineFont);
                float scaledBearingPx = bearingPx * line.fontScale;
                // Use unscaled bearingPx as the floor (matching renderParagraph's baseH = max(inkH, bearingY))
                // so that scaled-down lines (e.g. fontScale=0.85 subtitle) do not overflow the
                // computed background bottom.
                float inkH = (line.imguiContent || line.select.has_value()) ? slotH
                                                                            : (ink.valid() ? std::max(ink.height() * line.fontScale, bearingPx) : scaledBearingPx);
                // ink.x1 is the visual right edge of the last glyph; +1px absorbs fp roundtrip loss in cell-division
                float inkW = (ink.valid() ? std::ceil(ink.x1) + 1.0f : textRenderer.MeasureWidth(line.prefix + line.text, textScale)) * line.fontScale;
                float totalW = leadingAdvanceCells + inkW / grid.cellSizeX;
                // Enforce minimum content width BEFORE adding the icon slot so lambdas
                // can return content-only widths and icon geometry stays encapsulated here.
                if (line.getMinContentWidthPx)
                    totalW = std::max(totalW, line.getMinContentWidthPx() / grid.cellSizeX);
                else if (line.select.has_value())
                {
                    const Select &sel = *line.select;
                    int n = static_cast<int>(sel.options.size());
                    if (n > 0)
                    {
                        ImFont *lblFont = bodyImFont ? bodyImFont : cachedTextImFont;
                        float selPad = ImGui::GetStyle().FramePadding.x;
                        float s = std::max(2.0f, std::round(slotH * ICON_SIZE_RATIO));
                        constexpr float iconGap = 3.0f;
                        float maxLblW = 0.0f;
                        for (const auto &opt : sel.options)
                            if (!opt.label.empty())
                                maxLblW = std::max(maxLblW, lblFont->CalcTextSizeA(lblFont->FontSize, FLT_MAX, 0.0f, opt.label.c_str()).x);
                        float titlePadW = line.text.empty() ? 0.0f : selPad * 3.0f;
                        // Only one zone (the active one) shows icon + label; the rest are icon-only.
                        float iconOnlyZoneW = 2.0f * s + 4.0f;
                        float maxLabelZoneW = 2.0f * s + (maxLblW > 0.0f ? iconGap + maxLblW : 0.0f) + 4.0f;
                        float minPillW = static_cast<float>(n - 1) * iconOnlyZoneW + maxLabelZoneW;
                        totalW = std::max(totalW, (inkW + titlePadW + minPillW) / grid.cellSizeX);
                    }
                }
                if (line.iconDraw)
                {
                    float sRatio = line.iconSizeRatio > 0.0f ? line.iconSizeRatio : ICON_SIZE_RATIO;
                    float s = std::max(2.0f, std::round(lineFont->FontSize * line.fontScale * sRatio));
                    totalW += (2.0f * s + 3.0f) / grid.cellSizeX;
                }
                maxBottom = std::max(maxBottom, lineStepPx * static_cast<float>(visibleI) + inkH);
                contentW = std::max(contentW, totalW);
                ++visibleI;
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
            contentH = item.header->para.box.outerHeight; // always full outerHeight: header margins unaffected
            contentW = item.header->para.box.outerWidth;
        }

        if (!item.collapsed)
        {
            bool isFirst = true;
            for (auto &child : item.children)
            {
                if (!child.visible)
                    continue;
                // tightHeader: absorb only the first child's top margin (header margins are kept intact).
                float childH = (isFirst && item.tightHeader) ? child.box.outerHeight - child.margin
                                                             : child.box.outerHeight;
                contentH += childH;
                contentW = std::max(contentW, child.box.outerWidth);
                isFirst = false;
            }
        }

        item.box.contentWidth = contentW;
        item.box.contentHeight = contentH;
        // Headerless sections are transparent containers: no margin/padding overhead.
        const float effMar = item.header.has_value() ? mar : 0.0f;
        const float effPad = item.header.has_value() ? pad : 0.0f;
        item.box.outerWidth  = 2.0f * effMar + contentW;        // Section has no background: padding is vertical-only
        item.box.outerHeight = 2.0f * effMar + effPad + contentH; // only top padding; last child's bottom margin provides bottom spacing
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

        if (item.subtitle.has_value())
        {
            computeParagraphBox(*item.subtitle);
            contentH += item.subtitle->box.outerHeight;
            contentW = std::max(contentW, item.subtitle->box.outerWidth);
        }

        // Horizontal panels lay children side-by-side: height = max of any child, NOT the sum.
        bool horizontal = item.horizontal;
        for (auto &child : item.children)
        {
            const UIElement &el = std::visit([](auto &e) -> const UIElement &
                                             { return e; }, child);
            if (!el.visible)
                continue;
            if (horizontal)
                contentH = std::max(contentH, el.box.outerHeight);
            else
            {
                contentH += el.box.outerHeight;
                contentW = std::max(contentW, el.box.outerWidth);
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
        // Headerless sections are transparent: no margin/padding inset.
        float insetX = parent.header.has_value() ? parent.margin : 0.0f;
        float cursor = parent.row + (parent.header.has_value() ? parent.margin + parent.padding : 0.0f);
        if (parent.header.has_value())
        {
            Paragraph &hpara = parent.header->para;
            hpara.col = parent.col + insetX;
            hpara.colSpan = parent.colSpan - 2.0f * insetX;
            hpara.row = cursor;
            hpara.rowSpan = hpara.box.outerHeight;
            updateLocalGrid(hpara, hpara.padding);
            cursor += hpara.box.outerHeight; // always full outerHeight: header margins unaffected by tightHeader
        }

        if (parent.collapsed)
            return;

        bool isFirst = true;
        float prevVisibleMargin = 0.0f;
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

            // tightHeader: back first child up by one margin so its content aligns with cursor.
            child.row = (isFirst && parent.tightHeader) ? cursor - child.margin : cursor;
            // List-like sections (e.g. prerequisites) collapse adjacent paragraph margins so
            // row-to-row spacing behaves more like paragraph line spacing.
            if (!isFirst && parent.noChildSplitters)
                child.row -= std::min(prevVisibleMargin, child.margin);
            child.rowSpan = child.box.outerHeight;
            updateLocalGrid(child, child.padding);

            // Advance cursor from the placed row to preserve any margin-collapsing adjustment.
            // When backed-up by tightHeader, net advance remains outerHeight - margin.
            cursor = child.row + ((isFirst && parent.tightHeader) ? child.box.outerHeight - child.margin
                                                                  : child.box.outerHeight);
            prevVisibleMargin = child.margin;
            isFirst = false;
        }
    };

    // Place children of a RootPanel (vertical stacking).
    auto placeChildrenVertical = [&](RootPanel &parent) -> void
    {
        if (parent.children.empty() && !parent.header.has_value() && !parent.subtitle.has_value())
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

        if (parent.subtitle.has_value())
        {
            Paragraph &sub = *parent.subtitle;
            sub.col = parent.col + insetX;
            sub.colSpan = parent.colSpan - 2.0f * insetX;
            sub.row = cursor;
            sub.rowSpan = sub.box.outerHeight;
            updateLocalGrid(sub, sub.padding);
            cursor += sub.box.outerHeight;
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
        if (!panel.children.empty() || panel.header.has_value() || panel.subtitle.has_value())
        {
            bool horizontal = panel.horizontal;

            if (horizontal)
            {
                float panelInset = panel.margin + panel.padding;

                // Place the header label on the left (natural width, vertically centred).
                float headerWidth = panelInset;
                if (panel.header.has_value())
                {
                    Paragraph &hpara = panel.header->para;
                    hpara.col = panel.col + panelInset;
                    hpara.colSpan = hpara.box.outerWidth;
                    hpara.row = panel.row + panelInset;
                    hpara.rowSpan = panel.rowSpan - 2.0f * panelInset;
                    updateLocalGrid(hpara, hpara.padding);
                    headerWidth += hpara.box.outerWidth;
                }

                // All children are tab sections stacked rightward from the header.
                float totalWidth = headerWidth;
                for (size_t i = 0; i < panel.children.size(); i++)
                {
                    const UIElement &el = std::visit([](const auto &e) -> const UIElement &
                                                     { return e; }, panel.children[i]);
                    if (!el.visible)
                        continue;
                    totalWidth += el.box.outerWidth;
                }

                if (!panel.rightAnchor && !panel.width)
                    panel.colSpan = std::max(panel.colSpan, totalWidth + panelInset);

                float currentCol = panel.col + headerWidth;
                for (size_t i = 0; i < panel.children.size(); i++)
                {
                    UIElement &el = std::visit([](auto &e) -> UIElement &
                                               { return e; }, panel.children[i]);
                    if (!el.visible)
                        continue;

                    el.col = currentCol;
                    el.colSpan = el.box.outerWidth;
                    el.row = panel.row + panel.margin + panel.padding; // inset by margin+padding — mirrors placeChildrenVertical
                    el.rowSpan = panel.rowSpan - 2.0f * (panel.margin + panel.padding);
                    updateLocalGrid(el, el.padding); // use the tab's own padding, not the panel's

                    currentCol += el.box.outerWidth;
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

    // StatusStrip spans Settings→Toolbar horizontally; it shares the top row with those panels until
    // here, so we shift Settings + Toolbar down to free the strip band (avoids forward-anchor cycles).
    {
        RootPanel *strip = GetPanel("StatusStrip");
        RootPanel *st = GetPanel("Settings");
        RootPanel *tb = GetPanel("Toolbar");
        if (strip && strip->visible && st && tb && st->visible && tb->visible)
        {
            const float gapBelowStripCells = UIGrid::GAP * 0.25f;
            const float dy = strip->rowSpan + gapBelowStripCells;
            st->row += dy;
            tb->row += dy;
            updateLocalGrid(*st, st->padding);
            placeChildrenVertical(*st);
            updateLocalGrid(*tb, tb->padding);
            placeChildrenVertical(*tb);
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
            bool isHorizontal = panel.horizontal;

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
        if (parent.subtitle.has_value())
            prevVisualBottom = parent.subtitle->row + parent.subtitle->rowSpan - parent.subtitle->margin;

        int visibleIdx = 0;
        for (const auto &child : parent.children)
        {
            const UIElement &el = std::visit([](const auto &e) -> const UIElement &
                                             { return e; }, child);
            if (!el.visible)
                continue;

            // Splitter between every visible child, and after the header/subtitle block.
            bool needsSplitter = (visibleIdx > 0) || ((parent.header.has_value() || parent.subtitle.has_value()) && visibleIdx == 0);
            // Place splitters between visible background edges (margin-aware),
            // so they land in the true inter-element gap.
            float childVisualTop = el.row + el.margin;
            if (std::holds_alternative<Section>(child))
            {
                const bool hasHeader = std::get<Section>(child).header.has_value();
                childVisualTop = hasHeader ? el.row + el.margin + el.padding : el.row;
            }
            if (needsSplitter)
                emitVerticalSplitter(prevVisualBottom, childVisualTop, bgX0, bgX1, parent.padding, Color::GetUI(colorLevel));

            // Background rect for Paragraphs that opt into one
            if (std::holds_alternative<Paragraph>(child))
            {
                const Paragraph &para = std::get<Paragraph>(child);
                if (para.bgParentDepth >= 0)
                {
                    float pmx = para.margin;
                    float px0 = grid.ToPixelsX(para.col + pmx);
                    float py0 = grid.ToPixelsY(para.row + pmx);
                    float px1 = grid.ToPixelsX(para.col + para.colSpan - pmx);
                    // Use resolved span (not content box) so stretched items keep correct visuals/gaps.
                    float py1 = grid.ToPixelsY(para.row + para.rowSpan - pmx);
                    float pr = std::min(grid.cellSizeX, grid.cellSizeY) * para.borderRadius;
                    EmitRoundedRect(vertices, indices, vertexOffset, px0, py0, px1, py1, pr, Color::GetUI(para.bgParentDepth + 1));
                }
            }

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
                    if (secVisibleIdx > 0 && !sec.noChildSplitters)
                        // Same depth as splitters between top-level panel children so nested parameter rows
                        // do not read as a brighter “second layer” than the row above.
                        emitVerticalSplitter(secPrevBottom, para.row + para.margin, secBgX0, secBgX1, parent.padding,
                                             Color::GetUI(colorLevel));
                    // Background rect for section-child Paragraphs that opt into one
                    if (para.bgParentDepth >= 0)
                    {
                        float pmx = para.margin;
                        float px0 = grid.ToPixelsX(para.col + pmx);
                        float py0 = grid.ToPixelsY(para.row + pmx);
                        float px1 = grid.ToPixelsX(para.col + para.colSpan - pmx);
                        // Use resolved span (not content box) so stretched items keep correct visuals/gaps.
                        float py1 = grid.ToPixelsY(para.row + para.rowSpan - pmx);
                        float pr = std::min(grid.cellSizeX, grid.cellSizeY) * para.borderRadius;
                        EmitRoundedRect(vertices, indices, vertexOffset, px0, py0, px1, py1, pr, Color::GetUI(para.bgParentDepth + 1));
                    }
                    secPrevBottom = para.row + para.rowSpan - para.margin;
                    secVisibleIdx++;
                }
            }

            // Advance to the same margin-aware visual boundary we use for splitter placement.
            if (std::holds_alternative<Section>(child))
            {
                const bool hasHeader = std::get<Section>(child).header.has_value();
                prevVisualBottom = hasHeader ? el.row + el.rowSpan - el.margin - el.padding
                                             : el.row + el.rowSpan;
            }
            else
                prevVisualBottom = el.row + el.rowSpan - el.margin;
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
            // Same nominal radius as other root panels, but cap by half the shorter side so
            // shallow strips (e.g. status bar) do not read as full pills; tall panels unchanged.
            const float wPx = x1 - x0;
            const float hPx = y1 - y0;
            if (wPx > 1.0f && hPx > 1.0f)
                r = std::min(r, 0.5f * std::min(wPx, hPx));
            EmitRoundedRect(vertices, indices, vertexOffset, x0, y0, x1, y1, r, Color::GetUI(panel.bgParentDepth + 1));
        }

        bool horizontal = panel.horizontal;
        if (horizontal)
        {
            // Vertical separator lines between tabs: span the panel padding rect (margin+padding inset).
            float y0 = grid.ToPixelsY(panel.row + panel.margin + panel.padding);
            float y1 = grid.ToPixelsY(panel.row + panel.rowSpan - panel.margin - panel.padding);
            float halfPadPx = grid.ToPixelsX(panel.padding) * 0.5f;
            // Splitter between header and first child.
            if (panel.header.has_value() && !panel.children.empty())
            {
                float halfSpl = SPLITTER_HEIGHT * 0.5f;
                float splX = panel.header->para.col + panel.header->para.colSpan;
                float rsx0 = std::round(grid.ToPixelsX(splX - halfSpl));
                float rsx1 = std::round(grid.ToPixelsX(splX + halfSpl));
                float rsy0 = std::round(y0 + halfPadPx);
                float rsy1 = std::round(y1 - halfPadPx);
                float sr = std::min(rsx1 - rsx0, rsy1 - rsy0) * 0.5f;
                EmitRoundedRect(vertices, indices, vertexOffset, rsx0, rsy0, rsx1, rsy1, sr, Color::GetUI(2));
            }

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
    auto renderParagraph = [&](Paragraph &item, const std::string &itemPath) -> void
    {
        if (!item.visible)
            return;

        const auto &slg = item.localGrid;
        float bearingY = pixelImFont ? pixelImFont->FontSize
                                     : slg.cellSizeY;
        // imguiContent lines need a slot tall enough for GetFrameHeight().
        float slotH = bearingY;
        if (pixelImFont)
        {
            for (const auto &v : item.values)
                if (v.imguiContent || v.select.has_value())
                {
                    slotH = std::max(slotH, pixelImFont->FontSize + ImGui::GetStyle().FramePadding.y * 2.0f);
                    break;
                }
        }
        float lineStepPx = item.lineGap * grid.cellSizeY + slotH;
        float contentPx = grid.ToPixelsX(item.col + item.margin + item.padding);
        float contentPy = grid.ToPixelsY(item.row + item.margin + item.padding);
        float leadingPx = grid.ToPixelsX(item.leadingWidth); // 0 when no leading slot

        // Paragraph bounding box (margin-inset) — used for fills, accent bar, and paragraph-level hit area.
        float mx  = item.margin;
        float px0 = grid.ToPixelsX(item.col + mx);
        float py0 = grid.ToPixelsY(item.row + mx);
        float px1 = grid.ToPixelsX(item.col + item.colSpan - mx);
        float py1 = grid.ToPixelsY(item.row + item.rowSpan - mx);
        float pr  = std::min(grid.cellSizeX, grid.cellSizeY) * item.borderRadius;

        // Paragraph-level fills and accent bar — drawn before per-line windows,
        // spanning the full paragraph background area (margin-inset).
        if (item.dimFill || item.accentBar || item.selected)
        {
            ImDrawList *dl = ImGui::GetForegroundDrawList();
            if (item.dimFill)
            {
                glm::vec4 bg = Color::GetUIText(1);
                dl->AddRectFilled(ImVec2(px0, py0), ImVec2(px1, py1),
                                  ImGui::GetColorU32(ImVec4(bg.r, bg.g, bg.b, 0.07f)), pr);
            }
            if (item.selected)
            {
                glm::vec4 bg = Color::GetAccent(item.layer + 1, 0.15f, ACCENT_SAT_MULT_HOVER);
                dl->AddRectFilled(ImVec2(px0, py0), ImVec2(px1, py1),
                                  ImGui::GetColorU32(ImVec4(bg.r, bg.g, bg.b, bg.a)), pr);
            }
            if (item.accentBar)
            {
                constexpr float barW = 4.0f;
                glm::vec4 ac = Color::GetAccent(2, 1.0f, 1.0f);
                // Bar sits in the margin zone (left of the card background) so that padding
                // is the equal gap from the card edge to text on all four sides.
                dl->AddRectFilled(ImVec2(px0 - barW, py0),
                                  ImVec2(px0, py1),
                                  ImGui::GetColorU32(ImVec4(ac.r, ac.g, ac.b, ac.a)),
                                  pr, ImDrawFlags_RoundCornersLeft);
            }
        }

        // Leading slot — height derived from font bearing metrics, not ink bounds,
        // so it spans the full typographic extent regardless of which glyphs are present.
        constexpr float LEADING_GAP_PX = 4.0f; // fixed gap between checkbox and text
        float usedLeadingPx = leadingPx;        // actual advance + btnW deduction
        if (item.leadingDraw && leadingPx > 0.0f)
        {
            size_t visibleLineCount = 0;
            for (const auto &v : item.values)
                if (v.visible) ++visibleLineCount;
            float ly0 = contentPy;
            float ly1 = contentPy + lineStepPx * static_cast<float>(visibleLineCount > 0 ? visibleLineCount - 1 : 0) + slotH;
            ImDrawList *dl = ImGui::GetForegroundDrawList();
            // Checkbox width = single-line slot height → square on one-line, taller on two-line.
            float checkboxW = std::min(slotH, leadingPx);
            item.leadingDraw(dl, contentPx, ly0, contentPx + checkboxW, ly1);
            usedLeadingPx = checkboxW + LEADING_GAP_PX;
            contentPx += usedLeadingPx;
        }

        size_t visibleI = 0;
        for (size_t i = 0; i < item.values.size(); i++)
        {
            auto &line = item.values[i];
            if (!line.visible)
                continue;
            // Measure actual glyph ink bounds at add-text origin (0,0).
            // onClick lines use pixelImFont; bold lines use the heavy context font;
            // body lines use the lighter bodyImFont if available.
            ImFont *font = (line.onClick && pixelImFont) ? pixelImFont
                           : (!line.bold && bodyImFont)  ? bodyImFont
                                                         : ImGui::GetFont();
            float renderSize = font->FontSize * line.fontScale;
            PixelBounds ink = MeasureImGuiInkBounds(line.prefix + line.text, font);
            float inkH = (ink.valid() ? ink.height() : bearingY) * line.fontScale;
            float inkY0 = (ink.valid() ? ink.y0 : 0.0f) * line.fontScale;

            float vpx = contentPx;
            float vpy = contentPy + bearingY + lineStepPx * static_cast<float>(visibleI);

            float baseH = (line.imguiContent || line.select.has_value()) ? slotH : std::max(inkH, bearingY);
            float btnX = vpx;
            float btnY = vpy - bearingY;
            float btnW = (slg.columns - 2.0f * slg.padding) * slg.cellSizeX - usedLeadingPx;
            const bool useSquareHit =
                line.squareIconHit && line.iconDraw && line.onClick && !item.onClick && !line.imguiContent &&
                !line.select.has_value() && line.text.empty() && line.prefix.empty();
            float winX = btnX;
            float winY = btnY;
            float winW = btnW;
            float winH = baseH;
            if (useSquareHit)
            {
                float side = std::min(btnW, baseH);
                winX = btnX + (btnW - side) * 0.5f;
                winY = btnY + (baseH - side) * 0.5f;
                winW = side;
                winH = side;
            }

            // Compute icon slot offset and draw icon if present.
            // The ImGui window covers the row (or a centered square for toolbar tools) so clicks register.
            float iconOffset = 0.0f;
            if (line.iconDraw)
            {
                float sRatio = line.iconSizeRatio > 0.0f ? line.iconSizeRatio : ICON_SIZE_RATIO;
                float s = std::max(2.0f, std::round(font->FontSize * line.fontScale * sRatio));
                iconOffset = 2.0f * s + 3.0f;
                float midY = useSquareHit ? (winY + winH * 0.5f) : (btnY + baseH * 0.5f);
                float iconLeft = useSquareHit ? (winX + (winW - iconOffset) * 0.5f) : vpx;
                ImDrawList *dl = ImGui::GetForegroundDrawList();
                line.iconDraw(dl, iconLeft, midY, s);
            }

            ImGui::SetNextWindowPos(ImVec2(winX, winY));
            ImGui::SetNextWindowSize(ImVec2(winW, winH));
            std::string winId = "##val" + itemPath + "_" + std::to_string(i);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            if (ImGui::Begin(winId.c_str(), nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings))
            {
                if (line.select.has_value())
                {
                    // Segmented pill selector: N equal zones, accent pill behind active zone.
                    Select &sel = *line.select;
                    int n = static_cast<int>(sel.options.size());
                    if (n > 0)
                    {
                        float pad = ImGui::GetStyle().FramePadding.x;
                        ImFont *lblFont = bodyImFont ? bodyImFont : ImGui::GetFont();
                        ImDrawList *dl = ImGui::GetWindowDrawList();

                        // Optional left-side row title stored in SectionLine::text
                        float titleW = 0.0f;
                        if (!line.text.empty())
                        {
                            float lblSz = lblFont->FontSize;
                            ImVec2 ts = lblFont->CalcTextSizeA(lblSz, FLT_MAX, 0.0f, line.text.c_str());
                            titleW = ts.x + pad * 3.0f; // label + right gap before pill
                            glm::vec4 tc = Color::GetUIText(2);
                            ImU32 col = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
                            float ty = btnY + (baseH - lblSz) * 0.5f;
                            dl->AddText(lblFont, lblSz, ImVec2(btnX + iconOffset + pad, ty), col, line.text.c_str());
                        }

                        float pillAreaX = btnX + titleW; // zones start after title
                        float pillAreaW = btnW - titleW;
                        float pillR = std::round(baseH * 0.35f);
                        float s = std::max(2.0f, std::round(baseH * ICON_SIZE_RATIO));
                        constexpr float iconGap = 3.0f;
                        constexpr float zoneInset = 2.0f; // per side (2×2 = 4px total, matching layout)
                        float lblSize = lblFont->FontSize;

                        // Variable-width zones: active = icon + label, inactive = icon only.
                        // Any surplus width from the available pill area goes to the active zone.
                        std::vector<float> zoneWidths(n);
                        float totalNatural = 0.0f;
                        for (int zi = 0; zi < n; ++zi)
                        {
                            bool isActive = (zi == sel.activeIndex);
                            float lw = 0.0f;
                            if (isActive && !sel.options[zi].label.empty())
                                lw = lblFont->CalcTextSizeA(lblSize, FLT_MAX, 0.0f, sel.options[zi].label.c_str()).x;
                            zoneWidths[zi] = 2.0f * s + (lw > 0.0f ? iconGap + lw : 0.0f) + 2.0f * zoneInset;
                            totalNatural += zoneWidths[zi];
                        }
                        float surplus = pillAreaW - totalNatural;
                        if (surplus > 0.0f)
                        {
                            // Distribute surplus only to inactive (icon-only) zones so the
                            // active zone width is purely content-driven and stays the same
                            // across rows that share the same column (e.g. Theme vs Accent).
                            int nInactive = n - 1; // exactly one active zone
                            if (nInactive > 0)
                            {
                                float perInactive = surplus / static_cast<float>(nInactive);
                                for (int zi = 0; zi < n; ++zi)
                                    if (zi != sel.activeIndex)
                                        zoneWidths[zi] += perInactive;
                            }
                        }

                        float cumX = 0.0f;
                        for (int zi = 0; zi < n; ++zi)
                        {
                            float thisW = zoneWidths[zi];
                            float zx0 = pillAreaX + cumX;
                            float zx1 = zx0 + thisW;
                            bool isActive = (zi == sel.activeIndex);

                            // Invisible button for click detection (local window coords)
                            ImGui::SetCursorPos(ImVec2(titleW + cumX, 0));
                            std::string btnId = "##sel" + std::to_string(i) + "_" + std::to_string(zi);
                            ImGui::InvisibleButton(btnId.c_str(), ImVec2(thisW, baseH));
                            bool hovered = ImGui::IsItemHovered();
                            bool clicked = ImGui::IsItemClicked();
                            if (hovered)
                                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

                            // Pill fill: active = accent, hovered-inactive = subtle tint
                            if (isActive)
                            {
                                glm::vec4 ac = Color::GetAccent(item.layer + 1, 1.0f, ACCENT_SAT_MULT_HOVER);
                                dl->AddRectFilled(ImVec2(zx0 + 2.f, btnY + 2.f), ImVec2(zx1 - 2.f, btnY + baseH - 2.f),
                                                  ImGui::GetColorU32(ImVec4(ac.r, ac.g, ac.b, ac.a)), pillR);
                            }
                            else if (hovered)
                            {
                                glm::vec4 hc = Color::GetAccent(item.layer + 1, 0.12f, ACCENT_SAT_MULT_HOVER);
                                dl->AddRectFilled(ImVec2(zx0 + 2.f, btnY + 2.f), ImVec2(zx1 - 2.f, btnY + baseH - 2.f),
                                                  ImGui::GetColorU32(ImVec4(hc.r, hc.g, hc.b, hc.a)), pillR);
                            }

                            float midZone = (zx0 + zx1) * 0.5f;
                            float midRow = btnY + baseH * 0.5f;
                            int depth = isActive ? 2 : 0;

                            float labelW = 0.0f;
                            if (isActive && !sel.options[zi].label.empty())
                                labelW = lblFont->CalcTextSizeA(lblSize, FLT_MAX, 0.0f, sel.options[zi].label.c_str()).x;

                            // Centre icon + label together within the zone
                            float contentW = 2.0f * s + (labelW > 0.0f ? iconGap + labelW : 0.0f);
                            float contentStartX = midZone - contentW * 0.5f;

                            // Clip to zone interior so nothing overflows
                            dl->PushClipRect(ImVec2(zx0 + 2.f, btnY), ImVec2(zx1 - 2.f, btnY + baseH), true);

                            if (sel.options[zi].iconDraw)
                                sel.options[zi].iconDraw(dl, contentStartX, midRow, s);

                            if (isActive && labelW > 0.0f)
                            {
                                float ty = btnY + (baseH - lblSize) * 0.5f;
                                glm::vec4 tc = Color::GetUIText(depth);
                                ImU32 lblCol = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
                                dl->AddText(lblFont, lblSize, ImVec2(contentStartX + 2.0f * s + iconGap, ty), lblCol, sel.options[zi].label.c_str());
                            }

                            dl->PopClipRect();

                            if (clicked && !isActive)
                            {
                                sel.activeIndex = zi;
                                if (sel.onChange)
                                    sel.onChange(zi);
                            }
                            else if (clicked && isActive)
                            {
                                if (sel.onActiveClick)
                                    sel.onActiveClick();
                            }

                            cumX += thisW;
                        }
                        if (sel.postDraw)
                            sel.postDraw();
                    }
                }
                else if (line.imguiContent)
                {
                    if (pixelImFont)
                        ImGui::PushFont(pixelImFont);
                    ImGui::SetCursorPos(ImVec2(0, 0)); // ensure cursor starts at top-left of slot window
                    line.imguiContent(winW, winH, iconOffset); // matches ImGui window size
                    if (pixelImFont)
                        ImGui::PopFont();
                }
                else
                {
                    // When the paragraph has a unified onClick, skip per-line InvisibleButton;
                    // the paragraph-level window (submitted after all line windows) handles all input.
                    if (line.onClick && !item.onClick)
                    {
                        ImGui::SetCursorPos(ImVec2(0, 0));
                        if (ImGui::InvisibleButton(("##btn" + std::to_string(i)).c_str(), ImVec2(winW, winH)))
                            line.onClick();
                        if (ImGui::IsItemHovered())
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    }
                    // Centre the ink region in the visual slot
                    float ty = btnY + (baseH - inkH) * 0.5f - inkY0;
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    float rowRadius = std::min(winW, winH) * UIStyle::FRAME_ROUNDING_RATIO;
                    // Neutral dim fill — completed step
                    if (line.dimFill)
                    {
                        glm::vec4 bg = Color::GetUIText(1);
                        dl->AddRectFilled(ImVec2(winX, winY), ImVec2(winX + winW, winY + winH),
                                          ImGui::GetColorU32(ImVec4(bg.r, bg.g, bg.b, 0.07f)), rowRadius);
                    }
                    // Accent fill — hover/active for per-line clickable lines only
                    {
                        bool hovered = line.onClick && !item.onClick && ImGui::IsItemHovered();
                        bool active  = line.onClick && !item.onClick && ImGui::IsItemActive();
                        if (hovered || active || line.selected)
                        {
                            int d = active ? item.layer + 2 : item.layer + 1;
                            float alpha = active ? 0.18f : line.selected ? 0.20f
                                                                         : 0.10f;
                            glm::vec4 bg = Color::GetAccent(d, alpha, ACCENT_SAT_MULT_HOVER);
                            dl->AddRectFilled(ImVec2(winX, winY), ImVec2(winX + winW, winY + winH),
                                              ImGui::GetColorU32(ImVec4(bg.r, bg.g, bg.b, bg.a)), rowRadius);
                        }
                    }
                    // Left accent bar — active step indicator, drawn over fills
                    if (line.accentBar)
                    {
                        constexpr float barW = 3.0f;
                        glm::vec4 ac = Color::GetAccent(2, 1.0f, 1.0f);
                        dl->AddRectFilled(ImVec2(winX + 1.0f, winY + 2.0f),
                                          ImVec2(winX + 1.0f + barW, winY + winH - 2.0f),
                                          ImGui::GetColorU32(ImVec4(ac.r, ac.g, ac.b, ac.a)),
                                          barW * 0.5f);
                    }
                    float tx = winX + iconOffset; // skip icon slot before drawing text
                    if (!line.prefix.empty())
                    {
                        ImU32 pc = ImGui::GetColorU32(ImVec4(line.prefixColor.r, line.prefixColor.g, line.prefixColor.b, line.prefixColor.a));
                        dl->AddText(font, renderSize, ImVec2(tx, ty), pc, line.prefix.c_str());
                        tx += font->CalcTextSizeA(renderSize, FLT_MAX, 0.0f, line.prefix.c_str()).x;
                    }
                    if (!line.text.empty())
                    {
                        glm::vec4 tc = Color::GetUIText(line.textDepth);
                        ImU32 textCol = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
                        dl->AddText(font, renderSize, ImVec2(tx, ty), textCol, line.text.c_str());
                    }
                }
            }
            ImGui::End();
            ImGui::PopStyleVar(2);
            ++visibleI;
        }

        // Paragraph-level hit area — submitted last so it sits on top of all per-line windows.
        // Covers the full bounding box with a single InvisibleButton.
        if (item.onClick)
        {
            ImGui::SetNextWindowPos(ImVec2(px0, py0));
            ImGui::SetNextWindowSize(ImVec2(px1 - px0, py1 - py0));
            std::string paraWinId = "##para" + itemPath;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            if (ImGui::Begin(paraWinId.c_str(), nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings))
            {
                ImGui::SetCursorPos(ImVec2(0, 0));
                if (ImGui::InvisibleButton("##parabtn", ImVec2(px1 - px0, py1 - py0)))
                    item.onClick();
                bool paraHovered = ImGui::IsItemHovered();
                bool paraActive  = ImGui::IsItemActive();
                if (paraHovered || paraActive)
                {
                    int d       = paraActive ? item.layer + 2 : item.layer + 1;
                    float alpha = paraActive ? 0.18f : 0.10f;
                    glm::vec4 bg = Color::GetAccent(d, alpha, ACCENT_SAT_MULT_HOVER);
                    ImDrawList *fdl = ImGui::GetForegroundDrawList();
                    fdl->AddRectFilled(ImVec2(px0, py0), ImVec2(px1, py1),
                                       ImGui::GetColorU32(ImVec4(bg.r, bg.g, bg.b, bg.a)), pr);
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                }
            }
            ImGui::End();
            ImGui::PopStyleVar(2);
        }
    };

    // Render label for a Section header.
    auto renderSection = [&](Section &sec, const std::string &parentPath) -> void
    {
        if (!sec.visible)
            return;
        std::string secPath = parentPath + "_" + sec.id;

        if (sec.header.has_value())
        {
            // Wire chevron onto header line each frame (direction reflects current collapsed state).
            Paragraph &hpara = sec.header->para;
            if (!hpara.values.empty())
            {
                SectionLine &hl = hpara.values[0];
                hl.iconSizeRatio = ICON_SIZE_RATIO_SMALL;
                hl.iconDraw = Icons::Chevron(!sec.collapsed);
                hl.onClick = [&sec, this]()
                { sec.collapsed = !sec.collapsed; dirty = true; };
            }
            renderParagraph(hpara, secPath + "_header");
        }

        if (sec.collapsed)
            return;

        for (auto &para : sec.children)
            renderParagraph(para, secPath + "_" + para.id);
    };

    // Render text for all panels.
    for (auto &panel : panels)
    {
        if (!panel.visible)
            continue;

        std::string panelPath = "_" + panel.id;

        // Render RootPanel header (anonymous containers have no header)
        if (panel.header.has_value())
            renderParagraph(panel.header->para, panelPath + "_header");
        if (panel.subtitle.has_value())
            renderParagraph(*panel.subtitle, panelPath + "_subtitle");
        for (auto &child : panel.children)
        {
            std::visit([&](auto &el)
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
            // Match hit-testing: a hidden root panel must not contribute debug overlay
            // (children keep visible=true; only the panel flag is toggled for tool switch).
            if (!panel.visible)
                continue;

            drawDebugElement(panel);
            if (panel.header.has_value())
                drawDebugElement(panel.header->para);
            if (panel.subtitle.has_value())
                drawDebugElement(*panel.subtitle);

            for (const auto &child : panel.children)
            {
                std::visit([&](const auto &el)
                           {
                    if constexpr (std::is_same_v<std::decay_t<decltype(el)>, Section>)
                    {
                        if (!el.visible)
                            return;
                        drawDebugElement(el);
                        if (el.header.has_value())
                            drawDebugElement(el.header->para);
                        for (const auto &para : el.children)
                            drawDebugElement(para);
                    }
                    else
                    {
                        drawDebugElement(el);
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
