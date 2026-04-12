#pragma once

#include <glm/glm.hpp>
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
    std::string prefix;            // colored portion (e.g. the number)
    std::string text;              // default-colored portion (e.g. the label)
    glm::vec4 prefixColor{0};      // color for the prefix
    std::function<void()> onClick; // optional click callback
};

struct Panel
{
    // Layer-based border radius: panels are layer 0, sections are layer 1, etc.
    // Each layer gets 1 cell of total padding per axis (borderRadius = 0.5).
    static constexpr float BASE_RADIUS = 0.5f;  // border radius per layer (in global cells)
    static constexpr float RADIUS_DECAY = 1.0f; // multiplier per layer depth

    static constexpr float RadiusForLayer(int layer)
    {
        float r = BASE_RADIUS;
        for (int i = 0; i < layer; ++i)
            r *= RADIUS_DECAY;
        return r;
    }

    // Content padding per layer: constant across layers
    static constexpr float PaddingForLayer(int /*layer*/)
    {
        return 0.5f;
    }

    // Resolved position and size in grid cell units (computed by resolver)
    float col = 0;
    float row = 0;
    float colSpan = 0;
    float rowSpan = 0;
    float borderRadius = RadiusForLayer(0); // corner radius in cells (visual only)
    float padding = PaddingForLayer(0);     // content inset in global cells

    glm::vec4 color{0.0f};
    std::string id;
    std::vector<SectionLine> values; // multi-line content displayed below the section label
    bool showLabel = true;
    bool visible = true;
    bool showSplitter = true;

    // Optional ImGui content callback — when set, renders ImGui widgets inside the section
    std::function<void(float width, float height)> imguiContent;

    // Horizontal constraints
    std::optional<PanelAnchor> leftAnchor;  // defines left edge position
    std::optional<PanelAnchor> rightAnchor; // defines right edge position
    std::optional<float> width;             // fixed width in cells
    std::optional<float> minWidth;          // minimum width in cells (for stretching panels)

    // Vertical constraints
    std::optional<PanelAnchor> topAnchor;    // defines top edge position
    std::optional<PanelAnchor> bottomAnchor; // defines bottom edge position
    std::optional<float> height;             // fixed height in cells
    std::optional<float> minHeight;          // minimum height in cells (for stretching panels)

    // Local grid for panel content layout (computed by resolver)
    PanelGrid localGrid;

    // Child sections (stacked vertically with auto-splitters between them)
    std::vector<Panel> sections;

    Panel &AddSection(const std::string &sectionId)
    {
        Panel section;
        section.id = sectionId;
        section.borderRadius = RadiusForLayer(1);
        section.padding = PaddingForLayer(1);
        sections.push_back(section);
        return sections.back();
    }

    Panel &AddContent(const std::string &contentId)
    {
        Panel content;
        content.id = contentId;
        content.borderRadius = RadiusForLayer(2);
        content.padding = PaddingForLayer(2);
        content.showLabel = false;
        content.showSplitter = false;
        sections.push_back(content);
        return sections.back();
    }
};
