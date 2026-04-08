#include "scene.hpp"
#include "utils/log.hpp"

Point *Scene::CreatePoint(const glm::dvec3 &position)
{
    points.emplace_back(position);

    LOG_VOID("Created point");

    return &points.back();
}

Edge *Scene::CreateEdge(Point *startPoint, Point *endPoint)
{
    if (startPoint == nullptr)
    {
        LOG_WARN("Start point is null");
        return nullptr;
    }
    else if (endPoint == nullptr)
    {
        LOG_WARN("End point is null");
        return nullptr;
    }

    // Check if an edge between these two points already exists
    for (Edge *existing : startPoint->dependencies)
    {
        if ((existing->startPoint == startPoint && existing->endPoint == endPoint) ||
            (existing->startPoint == endPoint && existing->endPoint == startPoint))
        {
            return existing;
        }
    }

    edges.emplace_back(startPoint, endPoint);

    Edge &edge = edges.back();
    startPoint->dependencies.insert(&edge);
    endPoint->dependencies.insert(&edge);

    LOG_VOID("Created line edge");

    return &edge;
}

Edge *Scene::CreateEdge(Point *startPoint, Point *endPoint, Curve *curve)
{
    Edge *edge = CreateEdge(startPoint, endPoint);

    edge->curve = curve;
    curve->dependencies.insert(edge);

    LOG_VOID("Created line edge");

    return edge;
}

Edge *Scene::CreateEdge(Point *startPoint, Point *endPoint, const std::vector<Point *> &bridgePoints)
{
    Edge *edge = CreateEdge(startPoint, endPoint);

    edge->bridgePoints = bridgePoints;
    for (Point *point : bridgePoints)
        point->dependencies.insert(edge);

    LOG_VOID("Created poly line edge");

    return edge;
}

Curve *Scene::CreateCurve(glm::dvec3 centerPosition, double radius)
{
    ArcData arc;
    arc.center = centerPosition;
    arc.radius = radius;

    curves.push_back(std::make_unique<ArcCurve>(arc));

    LOG_VOID("Created arc curve");

    return curves.back().get();
}

Curve *Scene::CreateCurve(const tinynurbs::RationalCurve3d &nurbs)
{
    curves.push_back(std::make_unique<NurbsCurve>(
        std::make_unique<tinynurbs::RationalCurve3d>(nurbs)));

    LOG_VOID("Created nurbs curve");

    return curves.back().get();
}

Face *Scene::CreateFace(const std::vector<std::vector<Edge *>> &loops)
{
    faces.emplace_back(loops);

    Face &face = faces.back();

    // Register this face with all edges (now stored as OrientedEdge)
    for (const auto &loop : face.loops)
    {
        for (const auto &orientedEdge : loop)
            orientedEdge.edge->dependencies.insert(&face);
    }

    LOG_VOID("Created planar face");

    return &face;
}

Face *Scene::CreateFace(const std::vector<std::vector<Edge *>> &edgeLoops, const tinynurbs::RationalSurface3d &nurbs)
{
    faces.emplace_back(edgeLoops, std::make_unique<NurbsSurface>(
                                      std::make_unique<tinynurbs::RationalSurface3d>(nurbs)));

    Face &face = faces.back();

    // Register this face with all edges (now stored as OrientedEdge)
    for (const auto &loop : face.loops)
    {
        for (const auto &orientedEdge : loop)
            if (orientedEdge.edge != nullptr)
                orientedEdge.edge->dependencies.insert(&face);
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
        face->dependency = &solid;

    LOG_VOID("Created solid");

    return &solid;
}