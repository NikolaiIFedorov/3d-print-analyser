#pragma once

#include <cmath>

struct UIGrid
{
    static constexpr int COLUMNS = 80;
    static constexpr float GAP = 0.5f; // universal spacing in cells

    float cellSize = 0.0f;
    int columns = COLUMNS;
    int rows = 0;

    void Update(int screenWidth, int screenHeight)
    {
        cellSize = static_cast<float>(screenWidth) / static_cast<float>(columns);
        rows = static_cast<int>(std::floor(static_cast<float>(screenHeight) / cellSize));
    }

    float ToPixels(float cells) const { return cells * cellSize; }
    float ToPixelsX(float cells) const { return cells * cellSize; }
    float ToPixelsY(float cells) const { return cells * cellSize; }
};
