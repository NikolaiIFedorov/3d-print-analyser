#include "scene.hpp"
#include "utils/log.hpp"

Point *Scene::CreatePoint(const glm::dvec3 &position)
{
    points.emplace_back();

    Point &point = points.back();
    point.position = position;

    LOG_VOID("Created point");

    return &point;
}

Edge *Scene::CreateEdge(Point *startPoint, Point *endPoint, Curve *curve)
{
    edges.emplace_back();

    Edge &edge = edges.back();

    edge.startPoint = startPoint;
    startPoint->dependencies.insert(&edge);

    edge.endPoint = endPoint;
    endPoint->dependencies.insert(&edge);

    edge.curve = curve;
    curve->dependencies.insert(&edge);

    LOG_VOID("Created line edge");

    return &edge;
}

Edge *Scene::CreateEdge(Point *startPoint, Point *endPoint, const std::vector<Point *> &bridgePoints)
{
    edges.emplace_back();

    Edge &edge = edges.back();
    edge.startPoint = startPoint;
    startPoint->dependencies.insert(&edge);

    edge.endPoint = endPoint;
    endPoint->dependencies.insert(&edge);

    edge.bridgePoints = bridgePoints;
    for (Point *point : bridgePoints)
    {
        if (point)
        {
            point->dependencies.insert(&edge);
        }
    }

    LOG_VOID("Created poly line edge");

    return &edge;
}

Curve *Scene::CreateCurve(glm::dvec3 centerPosition, double radius)
{
    ArcData arc;
    arc.center = centerPosition;
    arc.radius = radius;

    curves.emplace_back();

    Curve &curve = curves.back();
    curve.data.operator=(arc);
    curve.type = CurveType::ARC;

    LOG_VOID("Created arc curve");

    return &curve;
}

Curve *Scene::CreateCurve(const tinynurbs::RationalCurve3d &nurbs)
{
    curves.emplace_back();

    Curve &curve = curves.back();
    curve.data = std::make_unique<tinynurbs::RationalCurve3d>(nurbs);
    curve.type = CurveType::NURBS;

    LOG_VOID("Created nurbs curve");

    return &curve;
}

Face *Scene::CreateFace(const std::vector<std::vector<Edge *>> &loops)
{
    faces.emplace_back();

    Face &face = faces.back();
    face.loops = loops;

    if (!loops.empty() && loops[0].size() >= 3)
    {
        const auto &outerLoop = loops[0];
        const Edge *e0 = outerLoop[0];
        const Edge *e1 = outerLoop[1];
        const Edge *e2 = outerLoop[2];

        if (e0 && e1 && e2)
        {
            const Point *p0 = e0->startPoint;
            const Point *p1 = e0->endPoint;
            const Point *p2 = e1->endPoint;

            if (p0 && p1 && p2)
            {
                glm::dvec3 v1 = p1->position - p0->position;
                glm::dvec3 v2 = p2->position - p0->position;
                glm::dvec3 normal = glm::normalize(glm::cross(v1, v2));

                double d = glm::dot(normal, p0->position);

                face.GetPlanar().normal = normal;
                face.GetPlanar().d = d;
            }
        }
    }

    for (std::vector<Edge *> loop : loops)
    {
        for (Edge *edge : loop)
            edge->dependencies.insert(&face);
    }

    LOG_VOID("Created planar face");

    return &face;
}

Face *Scene::CreateFace(const std::vector<std::vector<Edge *>> &edgeLoops, const tinynurbs::RationalSurface3d &nurbs)
{
    faces.emplace_back();

    Face &face = faces.back();
    face.loops = edgeLoops;
    face.surface = std::make_unique<tinynurbs::RationalSurface3d>(nurbs);

    for (std::vector<Edge *> loop : edgeLoops)
    {
        for (Edge *edge : loop)
            edge->dependencies.insert(&face);
    }

    LOG_VOID("Created nurbs face");

    return &face;
}

Solid *Scene::CreateSolid(const std::vector<Face *> &faces)
{
    solids.emplace_back();

    Solid &solid = solids.back();

    solid.faces = faces;
    for (Face *face : faces)
        face->dependencies.insert(&solid);

    LOG_VOID("Created solid");

    return &solid;
}