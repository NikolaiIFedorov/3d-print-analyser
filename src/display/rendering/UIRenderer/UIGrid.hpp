#pragma once

#include <cmath>

struct UIGrid
{
    static constexpr float CELL_SIZE = 40.0f; // fixed cell size in pixels
    static constexpr float GAP = 0.5f;        // universal spacing in cells
    /// Extra inset from the window bottom for screen-anchored `PanelAnchor::Bottom` (cells).
    /// Complements `GAP * 0.5f` so tall sidebars/toolbars do not sit flush on the frame.
    static constexpr float SCREEN_BOTTOM_INSET = 0.35f;

    float cellSizeX = CELL_SIZE;
    float cellSizeY = CELL_SIZE;

    // Dynamic extent — actual window size in cells
    float columns = 0.0f;
    float rows = 0.0f;

    // Minimum extent — derived from panel layout
    float minColumns = 0.0f;
    float minRows = 0.0f;

    void Update(int screenWidth, int screenHeight, float displayScale)
    {
        float scaledCell = CELL_SIZE * displayScale;
        cellSizeX = scaledCell;
        cellSizeY = scaledCell;
        columns = static_cast<float>(screenWidth) / scaledCell;
        rows = static_cast<float>(screenHeight) / scaledCell;
    }

    int MinWidthPixels() const { return static_cast<int>(minColumns * cellSizeX); }
    int MinHeightPixels() const { return static_cast<int>(minRows * cellSizeY); }

    float ToPixelsX(float cells) const { return cells * cellSizeX; }
    float ToPixelsY(float cells) const { return cells * cellSizeY; }
};
