#pragma once

#include <unordered_set>
struct Edge;

#include <glm/glm.hpp>
#include <TopoDS_Vertex.hxx>

struct Point
{
    std::unordered_set<Edge *> dependencies;
    TopoDS_Vertex occtVertex;
    Point() : position(0.0) {}

    glm::dvec3 position;

    Point(const glm::dvec3 &pos) : position(pos) {}
};