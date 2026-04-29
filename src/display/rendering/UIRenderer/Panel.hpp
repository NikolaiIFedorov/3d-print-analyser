#pragma once

#include <glm/glm.hpp>
#include <algorithm>
#include <cassert>
#include <cfloat>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "PanelGrid.hpp"
#include "UIGrid.hpp"
#include "Icons.hpp"

struct ImDrawList; // forward declaration for leftIconDraw callback

// Pixel-space bounding box built from actual or simulated vertex positions.
// Call expand() for every point (vertex corner, line endpoint, etc.) that gets drawn.
struct PixelBounds
{
    float x0 = FLT_MAX, y0 = FLT_MAX;
    float x1 = -FLT_MAX, y1 = -FLT_MAX;

    bool valid() const { return x0 <= x1 && y0 <= y1; }

    void expand(float x, float y)
    {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }

    void merge(const PixelBounds &o)
    {
        if (o.valid())
        {
            expand(o.x0, o.y0);
            expand(o.x1, o.y1);
        }
    }

    float width() const { return valid() ? x1 - x0 : 0.f; }
    float height() const { return valid() ? y1 - y0 : 0.f; }
};

struct RootPanel;
struct Section;
struct Paragraph;

// Any child element is either a Section or a Paragraph.
using ChildElement = std::variant<Section, Paragraph>;

struct PanelAnchor
{
    enum Edge
    {
        Left,
        Right,
        Top,
        Bottom
    };

    const RootPanel *panel = nullptr; // panel to reference (nullptr = screen)
    Edge edge = Left;
};

// Segmented pill selector — two or more labelled options in a single row.
// Rendered by UIRenderer as variable-width zones with an accent pill behind the active option.
struct SelectOption
{
    std::string label;      // short text shown on the active zone; inactive zones show icon only
    Icons::DrawFn iconDraw; // icon drawn in every zone
};

struct Select
{
    std::vector<SelectOption> options;   // ordered list of options
    int activeIndex = 0;                 // which option is currently selected
    std::function<void(int)> onChange;   // called with the new index when the user clicks a different zone
    std::function<void()> onActiveClick; // called when the already-active zone is clicked again
    std::function<void()> postDraw;      // called inside the row's ImGui window after all zones are drawn (e.g. popup hosting)
};

struct SectionLine
{
    std::string prefix;                                              // colored portion (e.g. the number)
    std::string text;                                                // default-colored portion (e.g. the label)
    glm::vec4 prefixColor{0};                                        // color for the prefix
    float fontScale = 1.0f;                                          // multiplier on font size (1.0 = body, 1.1 = section header, 1.25 = panel header)
    int textDepth = 1;                                               // Color::GetUIText depth (higher = brighter in dark, darker in light)
    std::function<void()> onClick;                                   // button attachment: underlined text, pointer cursor
    bool selected = false;                                           // persistent selected state — draws same background as hover
    bool accentBar = false;                                          // draws a left-edge accent bar (active step indicator)
    bool dimFill = false;                                            // draws a subtle neutral fill (completed step)
    std::function<void(ImDrawList *, float, float, float)> iconDraw; // icon attachment: drawn left of text; layout reserves slot automatically
    float iconSizeRatio = 0.0f;                                      // 0 = use global ICON_SIZE_RATIO; >0 overrides (e.g. ICON_SIZE_RATIO_SMALL for chevron)
    std::function<void(float, float, float)> imguiContent;           // input attachment: custom ImGui widget (w, h, iconOffsetPx)
    std::function<float()> getMinContentWidthPx;                     // optional: called at layout time to enforce min content width (px)
    bool bold = false;                                               // true = use heavy/title font; false = use body font if available
    std::optional<Select> select;                                    // segmented pill selector — replaces imguiContent when set
    /// Icon-only toolbar rows: use a centered square hit target and hover fill instead of stretching to full row width.
    bool squareIconHit = false;
    bool visible = true;                                             // false = skipped in layout and render
};

// Box model for UI elements (CSS-like). Each element has:
//   margin:  external spacing added between siblings and from parent
//   padding: internal spacing between background edge and content
//   lineGap: tighter spacing between stacked value lines
//
// Box sizes:
//   innerWidth/Height  = content extent
//   outerWidth/Height  = margin + padding + content + padding + margin
//   bgWidth/Height     = padding + content + padding  (background covers this)
//
// Margins are additive: space between two siblings = m1 + m2, from parent bg to first child bg = parent.padding + child.margin.
struct ContentBox
{
    float contentWidth = 0.0f;
    float contentHeight = 0.0f;
    float outerWidth = 0.0f; // margin + padding + content + padding + margin
    float outerHeight = 0.0f;
};

// Shared resolved-position fields. Populated by the layout resolver.
// Layer reflects nesting depth: RootPanel=0, Section=1, Paragraph=2.
struct UIElement
{
    static constexpr float BASE_RADIUS = 0.5f;
    static constexpr float RADIUS_STEP = 0.1f; // radius reduction per nesting layer
    static constexpr float LINE_GAP_RATIO = 0.35f;
    static constexpr float INSET_RATIO = 0.44f; // padding/margin as a fraction of UIGrid::GAP
    // Icon slot: s = max(2, round(lineBearing * ICON_SIZE_RATIO)); slot = 2s + 3 px wide
    static constexpr float ICON_SIZE_RATIO = 0.40f;
    static constexpr float ICON_SIZE_RATIO_SMALL = 0.28f; // chevron on collapsible section headers

    static constexpr float RadiusForLayer(int layer) { return BASE_RADIUS - RADIUS_STEP * layer; }
    static constexpr float PaddingForLayer(int layer) { return layer == 2 ? 0.0f : UIGrid::GAP * INSET_RATIO; }
    // Use uniform margin across layers for consistent inter-element gap visuals.
    static constexpr float MarginForLayer(int /*layer*/) { return UIGrid::GAP * INSET_RATIO; }

    std::string id;
    int layer = 0;

    // Resolved position and size in grid cell units (computed by resolver)
    float col = 0;
    float row = 0;
    float colSpan = 0;
    float rowSpan = 0;

    float borderRadius = RadiusForLayer(0);
    float margin = MarginForLayer(0);   // external: additive spacing between siblings
    float padding = PaddingForLayer(0); // internal: background edge to content
    float lineGap = 0.0f;               // spacing between value lines (cells, computed from padding * LINE_GAP_RATIO)

    bool visible = true;
    bool stretch = true; // false = use content width, centered in parent

    PanelGrid localGrid;
    ContentBox box;
};

// Paragraph — leaf element with interactive value lines. No children.
// Title text is expressed as a plain SectionLine in values; id is used only for lookup.
struct Paragraph : UIElement
{
    std::vector<SectionLine> values;
    int bgParentDepth = -1; // >= 0 draws a rounded-rect background; pass the parent element's depth (renderer adds +1)
    bool accentBar = false; // draws a left-edge accent bar spanning the full paragraph height
    bool dimFill = false;   // draws a subtle neutral fill over the full paragraph background
    bool selected = false;  // draws a persistent accent tint over the full paragraph background
    std::function<void(ImDrawList *, float, float, float, float)> leadingDraw; // optional full-height leading slot (x0,y0,x1,y1)
    float leadingWidth = 0.0f; // grid-cell width reserved left of all content lines (0 = no slot)
    std::function<void()> onClick; // optional: makes the leading slot a clickable button (hand cursor, InvisibleButton)

    Paragraph()
    {
        layer = 2;
        borderRadius = RadiusForLayer(2);
        margin = MarginForLayer(2);
        padding = PaddingForLayer(2);
    }
};

// Display label for a Section or RootPanel header.
// Backed by a Paragraph so it renders via the same ImGui overlay path as panel titles did originally.
// UIElement::id remains the lookup/debug key only.
struct Header
{
    Paragraph para;

    // fontScale: 1.0 = body, 1.1 = section header, 1.25 = panel header
    // textDepth: passed to Color::GetUIText — higher = brighter in dark mode
    Header(const std::string &text, float fontScale = 1.0f, int textDepth = 1)
    {
        std::string upper;
        upper.reserve(text.size());
        for (unsigned char c : text)
            upper += static_cast<char>(std::toupper(c));
        SectionLine &sl = para.values.emplace_back();
        sl.text = upper;
        sl.fontScale = fontScale;
        sl.textDepth = textDepth;
        sl.bold = true;
    }
};

// Section — labeled child container. No background (VS Code style). Can have Paragraph children.
// Splitters between children are drawn by the parent layout algorithm, not per-child flags.
struct Section : UIElement
{
    std::optional<Header> header; // present = render a title row above children
    std::vector<Paragraph> children;
    bool collapsed = false;        // true = children hidden, only header row rendered
    bool tightHeader = false;      // true = no gap between header title and first content row
    bool noChildSplitters = false; // true = suppress splitter lines between children (e.g. visually separated by their own backgrounds)

    Section()
    {
        layer = 1;
        borderRadius = RadiusForLayer(1);
        margin = 0.0f;
        padding = PaddingForLayer(1);
    }

    Paragraph &AddParagraph(const std::string &paragraphId)
    {
        assert(children.size() < children.capacity());
        Paragraph &p = children.emplace_back();
        p.id = paragraphId;
        return p;
    }
};

// RootPanel — top-level anchor-positioned container with background.
// Owns anchor constraints and a local grid. Children are Sections or Paragraphs.
struct RootPanel : UIElement
{
    int bgParentDepth{-1};             // >= 0 draws a background; set to parent element's depth (renderer adds +1 to get this panel's depth)
    std::optional<Header> header;      // present = render a title row above children
    std::optional<Paragraph> subtitle; // present = render a description row below the header with no splitter between them
    bool horizontal = false;           // true = lay children side-by-side (tab bar layout)

    // Horizontal constraints
    std::optional<PanelAnchor> leftAnchor;
    std::optional<PanelAnchor> rightAnchor;
    std::optional<float> width;
    std::optional<float> minWidth;

    // Vertical constraints
    std::optional<PanelAnchor> topAnchor;
    std::optional<PanelAnchor> bottomAnchor;
    std::optional<float> height;
    std::optional<float> minHeight;

    std::vector<ChildElement> children;

    RootPanel()
    {
        layer = 0;
        borderRadius = RadiusForLayer(0);
        margin = MarginForLayer(0);
        padding = PaddingForLayer(0);
    }

    bool HasBackground() const { return bgParentDepth >= 0; }

    Section &AddSection(const std::string &sectionId)
    {
        assert(children.size() < children.capacity());
        Section &s = std::get<Section>(children.emplace_back(Section{}));
        s.id = sectionId;
        return s;
    }

    Paragraph &AddParagraph(const std::string &paragraphId)
    {
        assert(children.size() < children.capacity());
        Paragraph &p = std::get<Paragraph>(children.emplace_back(Paragraph{}));
        p.id = paragraphId;
        return p;
    }
};
