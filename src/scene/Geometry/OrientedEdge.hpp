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
        if (edge == nullptr)
            return nullptr;
        return reversed ? edge->endPoint : edge->startPoint;
    }

    Point *GetEnd() const
    {
        if (edge == nullptr)
            return nullptr;
        return reversed ? edge->startPoint : edge->endPoint;
    }

    glm::dvec3 GetStartPosition() const
    {
        Point *p = GetStart();
        return p != nullptr ? p->position : glm::dvec3(0.0);
    }

    glm::dvec3 GetEndPosition() const
    {
        Point *p = GetEnd();
        return p != nullptr ? p->position : glm::dvec3(0.0);
    }
};
