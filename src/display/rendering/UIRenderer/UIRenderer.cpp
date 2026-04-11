#include "UIRenderer.hpp"
#include "rendering/color.hpp"
#include "utils/log.hpp"
#include <algorithm>
#include <cmath>

static constexpr float SPLITTER_HEIGHT = 0.2f; // splitter thickness between sections in cells

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
    }
}

void UIRenderer::SetSectionClick(const std::string &panelId, const std::string &sectionId, std::function<void()> onClick)
{
    // Auto-color the section if it has no background
    Panel *panel = GetPanel(panelId);
    if (panel)
    {
        for (auto &section : panel->sections)
        {
            if (section.id == sectionId && section.color.a <= 0.0f)
            {
                section.color = panel->color + glm::vec4(STEP, STEP, STEP, 0.0f);
                dirty = true;
            }
        }
    }
    sectionButtons.push_back({panelId, sectionId, std::move(onClick)});
}

void UIRenderer::SetSectionSlider(const std::string &panelId, const std::string &sectionId,
                                  double min, double max, double step, double *value,
                                  std::function<void()> onChange)
{
    sectionSliders.push_back({panelId, sectionId, min, max, step, value, std::move(onChange)});
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

    // Check section clicks first
    for (auto it = sectionButtons.rbegin(); it != sectionButtons.rend(); ++it)
    {
        const Panel *p = GetPanel(it->panelId);
        if (!p || !p->visible)
            continue;

        for (const auto &section : p->sections)
        {
            if (section.id == it->sectionId && section.visible)
            {
                if (cellX >= section.col && cellX <= section.col + section.colSpan &&
                    cellY >= section.row && cellY <= section.row + section.rowSpan)
                {
                    if (it->onClick)
                        it->onClick();
                    return true;
                }
            }
        }
    }

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

// Compute the pixel rect of the value text area within a slider section.
// Returns {vx0, vy0, vx1, vy1} — the bounds of the value button.
static void SliderValueRect(const Panel &section, const Panel &parent, const UIGrid &grid,
                            const TextRenderer &textRenderer, const std::string &valText,
                            float &vx0, float &vy0, float &vx1, float &vy1)
{
    float padPxH = grid.ToPixelsX(parent.padding);
    float padPxV = grid.ToPixelsY(parent.padding);
    float sx0 = grid.ToPixelsX(section.col) + padPxH;
    float sy0 = grid.ToPixelsY(section.row) + padPxV;
    float sx1 = grid.ToPixelsX(section.col + section.colSpan) - padPxH;
    float sy1 = grid.ToPixelsY(section.row + section.rowSpan) - padPxV;

    float localCell = grid.cellSizeX * PanelGrid::LOCAL_CELL_RATIO;
    float textScale = localCell / textRenderer.GetLineHeight(1.0f) * 1.4f;
    float valW = textRenderer.MeasureWidth(valText, textScale);
    float insetPx = section.localGrid.cellSizeX * section.padding;

    vx1 = sx1 - insetPx;
    vx0 = vx1 - valW - insetPx * 2.0f;
    vy0 = sy0;
    vy1 = sy1;
}

static double SnapToStep(double val, double min, double max, double step)
{
    double snapped = min + std::round((val - min) / step) * step;
    if (snapped < min)
        snapped = min;
    if (snapped > max)
        snapped = max;
    return snapped;
}

bool UIRenderer::HandleMouseDown(float pixelX, float pixelY)
{
    // If currently editing, a click anywhere commits the edit
    if (editingSlider)
    {
        // Apply the edit buffer
        char *end = nullptr;
        double parsed = std::strtod(editBuffer.c_str(), &end);
        if (end != editBuffer.c_str())
        {
            double newVal = SnapToStep(parsed, editingSlider->min, editingSlider->max, editingSlider->step);
            if (std::abs(newVal - *editingSlider->value) > 1e-12)
            {
                *editingSlider->value = newVal;
                if (editingSlider->onChange)
                    editingSlider->onChange();
            }
        }
        editingSlider = nullptr;
        editBuffer.clear();
        SDL_StopTextInput(window);
        dirty = true;
    }

    // Check sliders first — hit test on full section bounds
    for (auto &slider : sectionSliders)
    {
        const Panel *p = GetPanel(slider.panelId);
        if (!p || !p->visible)
            continue;

        for (const auto &section : p->sections)
        {
            if (section.id != slider.sectionId || !section.visible)
                continue;

            float sx0 = grid.ToPixelsX(section.col);
            float sy0 = grid.ToPixelsY(section.row);
            float sx1 = grid.ToPixelsX(section.col + section.colSpan);
            float sy1 = grid.ToPixelsY(section.row + section.rowSpan);

            if (pixelX >= sx0 && pixelX <= sx1 &&
                pixelY >= sy0 && pixelY <= sy1)
            {
                // Check if click is on the value text area
                std::string valText;
                if (!section.values.empty())
                    valText = section.values[0].prefix + section.values[0].text;

                if (!valText.empty())
                {
                    float vx0, vy0, vx1, vy1;
                    SliderValueRect(section, *p, grid, textRenderer, valText, vx0, vy0, vx1, vy1);

                    if (pixelX >= vx0 && pixelX <= vx1 &&
                        pixelY >= vy0 && pixelY <= vy1)
                    {
                        // Enter edit mode — select all
                        editingSlider = &slider;
                        editBuffer = valText;
                        editSelectAll = true;
                        SDL_StartTextInput(window);
                        dirty = true;
                        return true;
                    }
                }

                // Elsewhere on section → start drag
                activeSlider = &slider;
                lastDragX = pixelX;
                return true;
            }
        }
    }
    // Not on a slider — fall through to regular click handling
    return HandleClick(pixelX, pixelY);
}

bool UIRenderer::HandleMouseMotion(float pixelX, float pixelY)
{
    if (!activeSlider)
        return false;

    const Panel *p = GetPanel(activeSlider->panelId);
    if (!p)
    {
        activeSlider = nullptr;
        return false;
    }

    for (const auto &section : p->sections)
    {
        if (section.id != activeSlider->sectionId || !section.visible)
            continue;

        // Relative drag: dx maps to value change proportional to section width
        float sectionPx = grid.ToPixelsX(section.colSpan);
        double dx = static_cast<double>(pixelX - lastDragX);
        double rangePerPixel = (activeSlider->max - activeSlider->min) / static_cast<double>(sectionPx);
        double newVal = SnapToStep(*activeSlider->value + dx * rangePerPixel,
                                   activeSlider->min, activeSlider->max, activeSlider->step);
        lastDragX = pixelX;
        if (std::abs(newVal - *activeSlider->value) > 1e-12)
        {
            *activeSlider->value = newVal;
            if (activeSlider->onChange)
                activeSlider->onChange();
        }
        return true;
    }

    activeSlider = nullptr;
    return false;
}

void UIRenderer::HandleMouseUp()
{
    activeSlider = nullptr;
}

bool UIRenderer::HandleKeyDown(SDL_Keycode key)
{
    if (!editingSlider)
        return false;

    if (key == SDLK_RETURN || key == SDLK_KP_ENTER)
    {
        // Apply value
        char *end = nullptr;
        double parsed = std::strtod(editBuffer.c_str(), &end);
        if (end != editBuffer.c_str())
        {
            double newVal = SnapToStep(parsed, editingSlider->min, editingSlider->max, editingSlider->step);
            if (std::abs(newVal - *editingSlider->value) > 1e-12)
            {
                *editingSlider->value = newVal;
                if (editingSlider->onChange)
                    editingSlider->onChange();
            }
        }
        editingSlider = nullptr;
        editBuffer.clear();
        SDL_StopTextInput(window);
        dirty = true;
        return true;
    }
    else if (key == SDLK_ESCAPE)
    {
        // Cancel edit
        editingSlider = nullptr;
        editBuffer.clear();
        SDL_StopTextInput(window);
        dirty = true;
        return true;
    }
    else if (key == SDLK_BACKSPACE)
    {
        if (editSelectAll)
        {
            editBuffer.clear();
            editSelectAll = false;
        }
        else if (!editBuffer.empty())
        {
            editBuffer.pop_back();
        }
        dirty = true;
        return true;
    }
    return true; // consume all keys while editing
}

bool UIRenderer::HandleTextInput(const char *text)
{
    if (!editingSlider)
        return false;

    // If select-all is active, replace entire buffer
    if (editSelectAll)
    {
        editBuffer.clear();
        editSelectAll = false;
    }

    // Only allow digits, decimal point, minus sign
    for (const char *c = text; *c; ++c)
    {
        if ((*c >= '0' && *c <= '9') || *c == '.' || *c == '-')
            editBuffer += *c;
    }
    dirty = true;
    return true;
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
                auto hasSlider = [&](const std::string &sectionId) -> bool
                {
                    for (const auto &s : sectionSliders)
                        if (s.panelId == panel.id && s.sectionId == sectionId)
                            return true;
                    return false;
                };

                for (const auto &section : panel.sections)
                {
                    if (!section.visible)
                        continue;
                    float btnPad = (section.color.a > 0.0f) ? 2.0f * panel.padding : 0.0f;
                    float secLabelW = textRenderer.MeasureWidth(section.id, textScale) / grid.cellSizeX;
                    float secWidth = 2.0f * section.padding + secLabelW + btnPad;
                    if (hasSlider(section.id))
                    {
                        // Slider: label + gap + value on one line
                        float valW = 0;
                        for (const auto &line : section.values)
                        {
                            float lineW = textRenderer.MeasureWidth(line.prefix + line.text, textScale) / grid.cellSizeX;
                            valW = std::max(valW, lineW);
                        }
                        float gapCells = 1.0f;
                        secWidth = 2.0f * section.padding + secLabelW + gapCells + valW + btnPad;
                    }
                    else
                    {
                        for (const auto &line : section.values)
                        {
                            float lineW = textRenderer.MeasureWidth(line.prefix + line.text, textScale) / grid.cellSizeX;
                            float lineWidth = 2.0f * section.padding + lineW + btnPad;
                            secWidth = std::max(secWidth, lineWidth);
                        }
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
            float sectionHeight = 2.0f * panel.padding + textHeightCells;

            float textWidthPx = textRenderer.MeasureWidth(panel.id, textScale);
            float textWidthCells = textWidthPx / grid.cellSizeX;
            float sectionWidth = 2.0f * panel.padding + textWidthCells;

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
                // Each section height = label line + value lines
                float lineHeight = textHeightCells + 0.5f * secPadding; // line step in cells
                float totalHeight = sectionHeight;                      // header

                auto hasSlider = [&](const std::string &sectionId) -> bool
                {
                    for (const auto &s : sectionSliders)
                        if (s.panelId == panel.id && s.sectionId == sectionId)
                            return true;
                    return false;
                };

                for (const auto &section : panel.sections)
                {
                    if (!section.visible)
                        continue;
                    float btnPad = (section.color.a > 0.0f) ? 2.0f * panel.padding : 0.0f;
                    float secH = sectionHeight + btnPad; // base: label row + button padding
                    if (hasSlider(section.id))
                    {
                        // Slider: label+value on one line, no extra height
                        secH = sectionHeight;
                    }
                    else if (!section.values.empty())
                    {
                        float valLines = static_cast<float>(section.values.size());
                        secH = section.showLabel
                                   ? sectionHeight + valLines * lineHeight + btnPad
                                   : sectionHeight + (valLines - 1.0f) * lineHeight + btnPad;
                    }
                    totalHeight += (section.showSplitter ? SPLITTER_HEIGHT : 0.0f) + secH;
                }

                // Only expand if auto-sized (no bottom anchor and no fixed height)
                if (!panel.bottomAnchor && !panel.height)
                    panel.rowSpan = std::max(panel.rowSpan, totalHeight);

                float currentRow = panel.row + sectionHeight;
                for (auto &section : panel.sections)
                {
                    if (!section.visible)
                        continue;
                    float btnPad = (section.color.a > 0.0f) ? 2.0f * panel.padding : 0.0f;
                    float secH = sectionHeight + btnPad;
                    if (hasSlider(section.id))
                    {
                        secH = sectionHeight;
                    }
                    else if (!section.values.empty())
                    {
                        float valLines = static_cast<float>(section.values.size());
                        secH = section.showLabel
                                   ? sectionHeight + valLines * lineHeight + btnPad
                                   : sectionHeight + (valLines - 1.0f) * lineHeight + btnPad;
                    }

                    currentRow += section.showSplitter ? SPLITTER_HEIGHT : 0.0f;
                    section.col = panel.col;
                    section.colSpan = panel.colSpan;
                    section.row = currentRow;
                    section.rowSpan = secH;

                    float spx = grid.ToPixelsX(section.col);
                    float spy = grid.ToPixelsY(section.row);
                    float spw = grid.ToPixelsX(section.colSpan);
                    float sph = grid.ToPixelsY(section.rowSpan);
                    section.localGrid.Update(spx, spy, spw, sph, grid.cellSizeX, panel.padding);

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
                float sectionH = 2.0f * Panel::PaddingForLayer(1) + textHeightCells;
                float totalHeight = r.rowSpan;
                for (size_t i = 0; i < panel.sections.size(); i++)
                {
                    if (!panel.sections[i].visible)
                        continue;
                    float btnPad = (panel.sections[i].color.a > 0.0f) ? 2.0f * panel.padding : 0.0f;
                    totalHeight += (panel.sections[i].showSplitter ? SPLITTER_HEIGHT : 0.0f) + sectionH + btnPad;
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
                rsx0 = grid.ToPixelsX(section.col - SPLITTER_HEIGHT);
                rsx1 = grid.ToPixelsX(section.col);
                rsy0 = y0 + halfPadPx;
                rsy1 = y1 - halfPadPx;
            }
            else
            {
                // Horizontal splitter: above section, inset left/right
                rsx0 = x0 + halfPadPx;
                rsx1 = x1 - halfPadPx;
                rsy0 = grid.ToPixelsY(section.row - SPLITTER_HEIGHT);
                rsy1 = grid.ToPixelsY(section.row);
            }

            glm::vec4 splitterColor = Color::GetUI(2);
            float sr = std::min(rsx1 - rsx0, rsy1 - rsy0) * 0.5f; // fully rounded ends

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

        // Generate section background geometry (for sections with a color)
        for (const auto &section : panel.sections)
        {
            if (!section.visible || section.color.a <= 0.0f)
                continue;

            // Inset from parent panel edges and splitters
            float padPxH = grid.ToPixelsX(panel.padding);
            float padPxV = grid.ToPixelsY(panel.padding);
            float sx0 = grid.ToPixelsX(section.col) + padPxH;
            float sy0 = grid.ToPixelsY(section.row) + padPxV;
            float sx1 = grid.ToPixelsX(section.col + section.colSpan) - padPxH;
            float sy1 = grid.ToPixelsY(section.row + section.rowSpan) - padPxV;

            float sr = std::min(grid.cellSizeX, grid.cellSizeY) * section.borderRadius;
            float maxSR = std::min((sx1 - sx0), (sy1 - sy0)) * 0.5f;
            if (sr > maxSR)
                sr = maxSR;

            constexpr int SEGS = 8;
            float scx = (sx0 + sx1) * 0.5f;
            float scy = (sy0 + sy1) * 0.5f;

            vertices.push_back({{scx, scy}, section.color});
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
                    vertices.push_back({{spx, spy}, section.color});
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
        }

        // Generate slider value button backgrounds
        for (const auto &slider : sectionSliders)
        {
            if (slider.panelId != panel.id)
                continue;

            for (const auto &section : panel.sections)
            {
                if (section.id != slider.sectionId || !section.visible)
                    continue;
                if (section.values.empty())
                    break;

                std::string valText;
                bool isEditing = (&slider == editingSlider);
                if (isEditing && !editSelectAll)
                    valText = editBuffer + "|";
                else if (isEditing)
                    valText = editBuffer;
                else
                    valText = section.values[0].prefix + section.values[0].text;

                float vx0, vy0, vx1, vy1;
                SliderValueRect(section, panel, grid, textRenderer, valText, vx0, vy0, vx1, vy1);

                glm::vec4 btnColor;
                if (isEditing && editSelectAll)
                    btnColor = glm::vec4(0.2f, 0.35f, 0.55f, 1.0f); // selection accent
                else if (isEditing)
                    btnColor = Color::GetUI(3);
                else
                    btnColor = Color::GetUI(2);
                float sr = std::min(grid.cellSizeX, grid.cellSizeY) * section.borderRadius;
                float maxSR = std::min((vx1 - vx0), (vy1 - vy0)) * 0.5f;
                if (sr > maxSR)
                    sr = maxSR;

                constexpr int SEGS = 6;
                float cx = (vx0 + vx1) * 0.5f;
                float cy = (vy0 + vy1) * 0.5f;
                vertices.push_back({{cx, cy}, btnColor});
                uint32_t cIdx = vertexOffset++;

                struct Corner { float cx, cy, startAngle; };
                Corner corners[4] = {
                    {vx0 + sr, vy0 + sr, static_cast<float>(M_PI)},
                    {vx1 - sr, vy0 + sr, static_cast<float>(M_PI) * 1.5f},
                    {vx1 - sr, vy1 - sr, 0.0f},
                    {vx0 + sr, vy1 - sr, static_cast<float>(M_PI) * 0.5f},
                };
                uint32_t pStart = vertexOffset;
                for (int c = 0; c < 4; c++)
                    for (int s = 0; s <= SEGS; s++)
                    {
                        float angle = corners[c].startAngle +
                                      (static_cast<float>(M_PI) * 0.5f) *
                                          (static_cast<float>(s) / static_cast<float>(SEGS));
                        vertices.push_back({{corners[c].cx + sr * std::cos(angle),
                                             corners[c].cy + sr * std::sin(angle)},
                                            btnColor});
                        vertexOffset++;
                    }
                uint32_t total = 4 * (SEGS + 1);
                for (uint32_t i = 0; i < total; i++)
                {
                    indices.push_back(cIdx);
                    indices.push_back(pStart + i);
                    indices.push_back(pStart + (i + 1) % total);
                }
                break;
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

        // Render section labels and values
        for (const auto &section : panel.sections)
        {
            if (!section.visible)
                continue;
            const auto &slg = section.localGrid;
            float sTextScale = slg.cellSizeY / textRenderer.GetLineHeight(1.0f) * 1.4f;
            float bearingY = textRenderer.GetMaxBearingY(sTextScale);
            float lineStepPx = bearingY + slg.cellSizeY * 0.5f * section.padding;

            // Button sections: center text within the inset background
            // Check if this section has a slider first (sliders use label-left/value-right)
            bool isSlider = false;
            for (const auto &s : sectionSliders)
                if (s.panelId == panel.id && s.sectionId == section.id)
                {
                    isSlider = true;
                    break;
                }

            if (isSlider)
            {
                // Slider layout: label left in section, value right inside button
                float spx = slg.ToPixelsX(slg.padding);
                float spy = slg.ToPixelsY(slg.padding) + bearingY;

                if (!section.id.empty() && section.showLabel)
                    textRenderer.RenderText(section.id, spx, spy, sTextScale, Color::GetUIText(1));

                // Value inside the button background
                if (!section.values.empty())
                {
                    // Find the slider pointer for edit check
                    const SectionSlider *sl = nullptr;
                    for (const auto &s : sectionSliders)
                        if (s.panelId == panel.id && s.sectionId == section.id)
                        { sl = &s; break; }

                    bool editing = (sl == editingSlider && editingSlider != nullptr);
                    std::string valText;
                    if (editing && !editSelectAll)
                        valText = editBuffer + "|";
                    else if (editing)
                        valText = editBuffer;
                    else
                        valText = section.values[0].prefix + section.values[0].text;

                    float vx0, vy0, vx1, vy1;
                    SliderValueRect(section, panel, grid, textRenderer, valText, vx0, vy0, vx1, vy1);
                    float valW = textRenderer.MeasureWidth(valText, sTextScale);
                    float vpx = vx0 + (vx1 - vx0 - valW) * 0.5f;
                    float vpy = vy0 + (vy1 - vy0 + bearingY) * 0.5f;

                    if (editing)
                    {
                        textRenderer.RenderText(valText, vpx, vpy, sTextScale, Color::GetUIText(1));
                    }
                    else
                    {
                        const auto &line = section.values[0];
                        if (!line.prefix.empty())
                        {
                            textRenderer.RenderText(line.prefix, vpx, vpy, sTextScale, line.prefixColor);
                            vpx += textRenderer.MeasureWidth(line.prefix, sTextScale);
                        }
                        if (!line.text.empty())
                            textRenderer.RenderText(line.text, vpx, vpy, sTextScale, Color::GetUIText(1));
                    }
                }
            }
            else if (section.color.a > 0.0f)
            {
                float bgX0 = grid.ToPixelsX(section.col) + grid.ToPixelsX(panel.padding);
                float bgY0 = grid.ToPixelsY(section.row) + grid.ToPixelsY(panel.padding);
                float bgX1 = grid.ToPixelsX(section.col + section.colSpan) - grid.ToPixelsX(panel.padding);
                float bgY1 = grid.ToPixelsY(section.row + section.rowSpan) - grid.ToPixelsY(panel.padding);

                if (!section.id.empty() && section.showLabel)
                {
                    float tw = textRenderer.MeasureWidth(section.id, sTextScale);
                    float spx = bgX0 + (bgX1 - bgX0 - tw) * 0.5f;
                    float spy = bgY0 + (bgY1 - bgY0 + bearingY) * 0.5f;
                    textRenderer.RenderText(section.id, spx, spy, sTextScale, Color::GetUIText(1));
                }

                float valueStart = section.showLabel ? 1.0f : 0.0f;
                for (size_t i = 0; i < section.values.size(); i++)
                {
                    const auto &line = section.values[i];
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
                if (!section.id.empty() && section.showLabel)
                {
                    float spx = slg.ToPixelsX(slg.padding);
                    float spy = slg.ToPixelsY(slg.padding) + bearingY;
                    textRenderer.RenderText(section.id, spx, spy, sTextScale, Color::GetUIText(1));
                }

                float valueStart = section.showLabel ? 1.0f : 0.0f;
                for (size_t i = 0; i < section.values.size(); i++)
                {
                    const auto &line = section.values[i];
                    float vpx = slg.ToPixelsX(slg.padding);
                    float vpy = slg.ToPixelsY(slg.padding) + bearingY + lineStepPx * (valueStart + static_cast<float>(i));
                    if (!line.prefix.empty())
                    {
                        textRenderer.RenderText(line.prefix, vpx, vpy, sTextScale, line.prefixColor);
                        vpx += textRenderer.MeasureWidth(line.prefix, sTextScale);
                    }
                    if (!line.text.empty())
                        textRenderer.RenderText(line.text, vpx, vpy, sTextScale, Color::GetUIText(1));
                }
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
