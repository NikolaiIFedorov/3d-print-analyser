#pragma once

#include <unordered_set>
struct Face;

struct Curve;
#include <vector>
struct Point;

struct Edge
{
    std::unordered_set<Face *> dependencies;
    Edge() : startPoint(nullptr), endPoint(nullptr), curve(nullptr) {}

    Point *startPoint;
    Point *endPoint;
    std::vector<Point *> bridgePoints;
    Edge(Point *start, Point *end, std::vector<Point *> bridgePoints = {}) : startPoint(start), endPoint(end), bridgePoints(bridgePoints), curve(nullptr) {}

    Curve *curve;
    Edge(Point *start, Point *end, Curve *curve) : startPoint(start), endPoint(end), curve(curve) {}
};
