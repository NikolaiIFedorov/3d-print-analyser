#pragma once

#include <unordered_set>
struct Edge;

#include <glm/glm.hpp>

struct Point
{
    std::unordered_set<Edge *> dependencies;
    Point() : position(0.0) {}

    glm::dvec3 position;

    Point(const glm::dvec3 &pos) : position(pos) {}
};