#pragma once

// Local grid for laying out content within a panel.
// Cell size is smaller than the global grid (scaled by LOCAL_CELL_RATIO)
// to allow finer positioning of nested elements.
// Row 0 is the header section.
struct PanelGrid
{
    static constexpr float LOCAL_CELL_RATIO = 0.5f; // local cell = global cell * ratio

    float cellSizeX = 0.0f;
    float cellSizeY = 0.0f;

    // Content padding in local cells
    float padding = 0.0f;

    // Panel extent in local cells
    float columns = 0.0f;
    float rows = 0.0f;

    // Panel origin in pixels (top-left corner)
    float originX = 0.0f;
    float originY = 0.0f;

    void Update(float panelPixelX, float panelPixelY,
                float panelPixelW, float panelPixelH,
                float globalCellSize, float paddingGlobalCells)
    {
        originX = panelPixelX;
        originY = panelPixelY;
        float localCell = globalCellSize * LOCAL_CELL_RATIO;
        cellSizeX = localCell;
        cellSizeY = localCell;
        columns = panelPixelW / localCell;
        rows = panelPixelH / localCell;
        // Convert padding from global cells to local cells
        padding = paddingGlobalCells / LOCAL_CELL_RATIO;
    }

    // Convert local cell coordinates to pixel positions (absolute)
    float ToPixelsX(float cells) const { return originX + cells * cellSizeX; }
    float ToPixelsY(float cells) const { return originY + cells * cellSizeY; }
};
