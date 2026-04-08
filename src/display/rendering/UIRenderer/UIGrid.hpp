#pragma once

#include <cmath>

struct UIGrid
{
    static constexpr int COLUMNS = 80;
    static constexpr int ROWS = 45;
    static constexpr float GAP = 0.5f; // universal spacing in cells

    float cellSizeX = 0.0f;
    float cellSizeY = 0.0f;
    int columns = COLUMNS;
    int rows = ROWS;

    void Update(int screenWidth, int screenHeight)
    {
        cellSizeX = static_cast<float>(screenWidth) / static_cast<float>(columns);
        cellSizeY = static_cast<float>(screenHeight) / static_cast<float>(rows);
    }

    float ToPixelsX(float cells) const { return cells * cellSizeX; }
    float ToPixelsY(float cells) const { return cells * cellSizeY; }
};
