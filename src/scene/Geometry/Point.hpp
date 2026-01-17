#pragma once

#include <unordered_set>
struct Edge;

#include <glm/glm.hpp>

struct Point
{
    std::unordered_set<Edge *> dependencies;

    glm::dvec3 position;
};