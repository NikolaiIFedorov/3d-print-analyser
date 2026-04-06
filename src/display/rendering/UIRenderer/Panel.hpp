#pragma once

#include <glm/glm.hpp>
#include <optional>
#include <string>

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

struct Panel
{
    // Resolved position and size in grid cell units (computed by resolver)
    float col = 0;
    float row = 0;
    float colSpan = 0;
    float rowSpan = 0;
    float borderRadius = 1.0f; // corner radius in cells

    glm::vec4 color;
    std::string id;
    bool visible = true;

    // Horizontal constraints
    std::optional<PanelAnchor> leftAnchor;  // defines left edge position
    std::optional<PanelAnchor> rightAnchor; // defines right edge position
    std::optional<float> width;             // fixed width in cells

    // Vertical constraints
    std::optional<PanelAnchor> topAnchor;    // defines top edge position
    std::optional<PanelAnchor> bottomAnchor; // defines bottom edge position
    std::optional<float> height;             // fixed height in cells
};
