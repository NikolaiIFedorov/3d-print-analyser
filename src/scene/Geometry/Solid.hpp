#pragma once

#include <vector>
#include "AllGeometry.hpp"

struct Solid
{
    Solid() = default;

    std::vector<Face *> faces;
    Solid(Face *face) : faces({face}) {}
};