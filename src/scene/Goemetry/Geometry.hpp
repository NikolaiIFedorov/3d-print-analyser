#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <variant>

#include <glm/glm.hpp>
#include <tinynurbs/tinynurbs.h>

struct Vertex
{
    glm::vec3 position;
    glm::vec3 color;
};

struct ArcData
{
    glm::dvec3 center;
    double radius;
};

struct PlanarData
{
    glm::dvec3 normal;
    double d;
};

enum class CurveType
{
    ARC,
    NURBS
};
