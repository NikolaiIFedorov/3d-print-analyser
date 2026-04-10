#pragma once

#include <deque>
#include <unordered_set>
#include "Geometry/AllGeometry.hpp"

class Scene
{
public:
    std::deque<Point> points;
    Point *CreatePoint(const glm::dvec3 &position);

    std::deque<Edge> edges;
    Edge *CreateEdge(Point *startPoint, Point *endPoint);
    Edge *CreateEdge(Point *startPoint, Point *endPoint, Curve *curve);
    Edge *CreateEdge(Point *startPoint, Point *endPoint, const std::vector<Point *> &bridgePoints);

    std::deque<std::unique_ptr<Curve>> curves;
    Curve *CreateCurve(glm::dvec3 centerPoint, double radius);
    Curve *CreateCurve(const tinynurbs::RationalCurve3d &nurbs);

    std::deque<Face> faces;
    Face *CreateFace(const std::vector<std::vector<Edge *>> &edgeLoops);
    Face *CreateFace(const std::vector<std::vector<Edge *>> &edgeLoops, const tinynurbs::RationalSurface3d &nurbs);

    std::deque<Solid> solids;
    Solid *CreateSolid(const std::vector<Face *> &faces);

    void MergeCoplanarFaces(Solid *solid);

    std::unordered_set<uint32_t> renderBuffer;
    std::unordered_set<uint32_t> lockedBuffer;
};