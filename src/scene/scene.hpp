#pragma once

#include <unordered_set>
#include "Geometry/AllGeometry.hpp"

class Scene
{
public:
    std::vector<Point> points;
    Point *CreatePoint(const glm::dvec3 &position);

    std::vector<Edge> edges;
    Edge *CreateEdge(Point *startPoint, Point *endPoint, Curve *curve = 0);
    Edge *CreateEdge(Point *startPoint, Point *endPoint, const std::vector<Point *> &bridgePoints);

    std::vector<Curve> curves;
    Curve *CreateCurve(glm::dvec3 centerPoint, double radius);
    Curve *CreateCurve(const tinynurbs::RationalCurve3d &nurbs);

    std::vector<Face> faces;
    Face *CreateFace(const std::vector<std::vector<Edge *>> &edgeLoops);
    Face *CreateFace(const std::vector<std::vector<Edge *>> &edgeLoops, const tinynurbs::RationalSurface3d &nurbs);

    std::vector<Solid> solids;
    Solid *CreateSolid(const std::vector<Face *> &faces);

    std::unordered_set<uint32_t> renderBuffer;
    std::unordered_set<uint32_t> lockedBuffer;
};