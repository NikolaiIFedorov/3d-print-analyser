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

struct SectionLine
{
    std::string prefix;                             // colored portion (e.g. the number)
    std::string text;                               // default-colored portion (e.g. the label)
    glm::vec4 prefixColor{0};                       // color for the prefix
    float fontScale = 1.0f;                         // multiplier on font size (1.0 = body, 1.1 = section header, 1.25 = panel header)
    int textDepth = 1;                              // Color::GetUIText depth (higher = brighter in dark, darker in light)
    std::function<void()> onClick;                  // button attachment: underlined text, pointer cursor
    std::function<void(float, float)> imguiContent; // input attachment: custom ImGui widget (w, h)
    bool bold = false;                              // true = use heavy/title font; false = use body font if available
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
    static constexpr float LINE_GAP_RATIO = 0.35f;

    static constexpr float RadiusForLayer(int /*layer*/) { return BASE_RADIUS; }
    static constexpr float PaddingForLayer(int layer) { return layer == 2 ? 0.0f : 0.22f; }
    static constexpr float MarginForLayer(int /*layer*/) { return 0.22f; }

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
    bool collapsed = false; // true = children hidden, only header row rendered

    Section()
    {
        layer = 1;
        borderRadius = RadiusForLayer(1);
        margin = 0.0f;
        padding = PaddingForLayer(1);
    }

    Paragraph &AddParagraph(const std::string &paragraphId)
    {
        assert(children.capacity() == 0 || children.size() < children.capacity());
        Paragraph &p = children.emplace_back();
        p.id = paragraphId;
        return p;
    }
};

// RootPanel — top-level anchor-positioned container with background.
// Owns anchor constraints and a local grid. Children are Sections or Paragraphs.
struct RootPanel : UIElement
{
    int colorDepth{-1};           // >= 0 means draw a background via Color::GetUI(colorDepth)
    std::optional<Header> header; // present = render a title row above children

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

    bool HasBackground() const { return colorDepth >= 0; }

    Section &AddSection(const std::string &sectionId)
    {
        assert(children.capacity() == 0 || children.size() < children.capacity());
        Section &s = std::get<Section>(children.emplace_back(Section{}));
        s.id = sectionId;
        return s;
    }

    Paragraph &AddParagraph(const std::string &paragraphId)
    {
        assert(children.capacity() == 0 || children.size() < children.capacity());
        Paragraph &p = std::get<Paragraph>(children.emplace_back(Paragraph{}));
        p.id = paragraphId;
        return p;
    }
};
