#pragma once

#include <glm/glm.hpp>
#include <optional>
#include <string>

struct PanelAnchor
{
    enum Edge
    {
        Left,
        Right,
        Top,
        Bottom
    };

    std::string panelId; // panel to reference ("" = screen origin)
    Edge edge = Left;
    float offset = 0.0f; // offset in grid cells
};

struct Panel
{
    // All positions and sizes are in grid cell units
    float col;                 // x position in cells (overridden if colAnchor set)
    float row;                 // y position in cells (overridden if rowAnchor set)
    float colSpan;             // width in cells
    float rowSpan;             // height in cells
    float borderRadius = 1.0f; // corner radius in cells

    glm::vec4 color;
    std::string id;
    bool visible = true;

    bool anchorRight = false;  // colSpan stretches to fill remaining columns
    bool anchorBottom = false; // rowSpan stretches to fill remaining rows

    std::optional<PanelAnchor> colAnchor; // derive col from another panel's edge
    std::optional<PanelAnchor> rowAnchor; // derive row from another panel's edge
};
