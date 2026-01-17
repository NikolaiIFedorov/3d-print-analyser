#pragma once

#include <unordered_set>
struct Face;

struct Curve;
#include <vector>
struct Point;

struct Edge
{
    std::unordered_set<Face *> dependencies;

    Point *startPoint;
    Point *endPoint;

    std::vector<Point *> bridgePoints;
    Curve *curve;
};
