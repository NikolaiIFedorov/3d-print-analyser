#pragma once

#include <glm/glm.hpp>
#include <string>

struct Panel
{
    float x;
    float y;
    float width;
    float height;
    glm::vec4 color;
    std::string id;
    bool visible = true;

    bool anchorRight = false;  // width stretches to screen right edge
    bool anchorBottom = false; // height stretches to screen bottom edge
};
