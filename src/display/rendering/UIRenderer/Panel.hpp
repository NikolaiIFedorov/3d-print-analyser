#pragma once

#include <glm/glm.hpp>
#include <cassert>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "PanelGrid.hpp"

struct Panel;

struct PanelAnchor
{
    enum Edge
    {
        Left,
        Right,
        Top,
        Bottom
    };

    const Panel *panel = nullptr; // panel to reference (nullptr = screen)
    Edge edge = Left;
};

struct SectionLine
{
    std::string prefix;                             // colored portion (e.g. the number)
    std::string text;                               // default-colored portion (e.g. the label)
    glm::vec4 prefixColor{0};                       // color for the prefix
    std::function<void()> onClick;                  // button attachment: underlined text, pointer cursor
    std::function<void(float, float)> imguiContent; // input attachment: custom ImGui widget (w, h)
};

// Element kind — determines rendering behavior.
// The box model (padding, gap, margin collapsing) is the same for all kinds.
enum class UIKind
{
    Panel,     // top-level anchor-positioned container with background
    Section,   // child container (label + optional children; collapsible if flag set)
    Paragraph, // text leaf with interactive line attachments (onClick / imguiContent)
};

// Box model for UI elements (CSS-like). Each element has:
//   margin:  external spacing that collapses between siblings
//   padding: internal spacing between background edge and content
//   gap:     minimum spacing between child elements (owned by the parent)
//   lineGap: tighter spacing between stacked value lines
//
// Box sizes:
//   innerWidth/Height  = content extent
//   outerWidth/Height  = margin + padding + content + padding + margin
//   bgWidth/Height     = padding + content + padding  (background covers this)
//
// Between siblings, adjacent margins collapse: visual space = max(m1, m2, gap).
struct ContentBox
{
    float contentWidth = 0.0f;
    float contentHeight = 0.0f;
    float outerWidth = 0.0f; // margin + padding + content + padding + margin
    float outerHeight = 0.0f;
};

struct Panel
{
    // Layer-based border radius: panels are layer 0, sections are layer 1, etc.
    static constexpr float BASE_RADIUS = 0.5f;
    static constexpr float RADIUS_DECAY = 1.0f;

    static constexpr float RadiusForLayer(int /*layer*/)
    {
        return BASE_RADIUS;
    }

    static constexpr float PaddingForLayer(int /*layer*/)
    {
        return 0.25f;
    }

    static constexpr float MarginForLayer(int layer)
    {
        // Panels are anchor-positioned, not stacked — margin is structural 0.
        return layer == 0 ? 0.0f : 0.25f;
    }

    static constexpr float GapForLayer(int /*layer*/)
    {
        return 0.25f;
    }

    // Fraction of padding used as line-gap between stacked value lines
    static constexpr float LINE_GAP_RATIO = 0.35f;

    UIKind kind = UIKind::Panel;
    int layer = 0; // nesting depth: panel=0, section=1, paragraph=2

    // Resolved position and size in grid cell units (computed by resolver)
    float col = 0;
    float row = 0;
    float colSpan = 0;
    float rowSpan = 0;
    float borderRadius = RadiusForLayer(0);
    float margin = MarginForLayer(0);   // external: collapses between siblings
    float padding = PaddingForLayer(0); // internal: background edge to content
    float gap = GapForLayer(0);         // minimum spacing between child sections (cells)
    float lineGap = 0.0f;               // spacing between value lines (cells, computed from padding * LINE_GAP_RATIO)

    glm::vec4 color{0.0f};
    std::string id;
    std::vector<SectionLine> values;
    bool showLabel = true;
    bool visible = true;
    bool collapsible = false; // Section only: renderer ignores this on Panel/Paragraph
    bool showSplitter = true;
    bool stretch = true; // false = use content width, centered in parent

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

    PanelGrid localGrid;

    // Child elements (stacked vertically or horizontally)
    std::vector<Panel> sections;

    // Cached box model (recomputed during resolve)
    ContentBox box;

    bool HasBackground() const { return color.a > 0.0f; }
    bool IsContainer() const { return kind == UIKind::Panel || kind == UIKind::Section; }

    Panel &AddSection(const std::string &sectionId)
    {
        assert(sections.capacity() == 0 || sections.size() < sections.capacity());
        Panel section;
        section.kind = UIKind::Section;
        section.layer = 1;
        section.id = sectionId;
        section.borderRadius = RadiusForLayer(1);
        section.margin = MarginForLayer(1);
        section.padding = PaddingForLayer(1);
        section.gap = GapForLayer(1);
        sections.push_back(section);
        return sections.back();
    }

    Panel &AddParagraph(const std::string &paragraphId)
    {
        assert(sections.capacity() == 0 || sections.size() < sections.capacity());
        Panel paragraph;
        paragraph.kind = UIKind::Paragraph;
        paragraph.layer = 2;
        paragraph.id = paragraphId;
        paragraph.borderRadius = RadiusForLayer(2);
        paragraph.margin = MarginForLayer(2);
        paragraph.padding = PaddingForLayer(2);
        paragraph.gap = GapForLayer(2);
        paragraph.showLabel = false;
        paragraph.showSplitter = false;
        sections.push_back(paragraph);
        return sections.back();
    }
};
