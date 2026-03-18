#pragma once

#include "Edge.hpp"
#include "Point.hpp"
#include <glm/glm.hpp>

struct OrientedEdge
{
    Edge *edge;
    bool reversed;

    OrientedEdge(Edge *e, bool rev = false) : edge(e), reversed(rev) {}

    Point *GetStart() const
    {
        return reversed ? edge->endPoint : edge->startPoint;
    }

    Point *GetEnd() const
    {
        return reversed ? edge->startPoint : edge->endPoint;
    }

    glm::dvec3 GetStartPosition() const
    {
        return GetStart()->position;
    }

    glm::dvec3 GetEndPosition() const
    {
        return GetEnd()->position;
    }
};
