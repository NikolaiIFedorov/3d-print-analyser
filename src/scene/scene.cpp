#include "scene.hpp"
#include "utils/log.hpp"

uint32_t Scene::CreatePoint(const glm::dvec3 &position)
{
    points.emplace_back();

    Point &point = points.back();
    point.position = position;
    point.id = Id::GetId(Type::POINT);

    uint32_t id = point.id;
    uint32_t index = points.size() - 1;
    pointIdToIndex[id] = index;

    LOG_VOID("Created point");

    return id;
}

uint32_t Scene::CreateLineEdge(uint32_t startPointId, uint32_t endPointId, uint32_t curveId)
{
    edges.emplace_back();

    Edge &edge = edges.back();

    edge.startPointId = startPointId;
    edge.endPointId = endPointId;
    edge.curveId = curveId;
    edge.id = Id::GetId(Type::EDGE);

    uint32_t id = edge.id;
    uint32_t index = edges.size() - 1;
    edgeIdToIndex[id] = index;

    RegisterDependency(startPointId, id);
    RegisterDependency(endPointId, id);

    LOG_VOID("Created line edge");

    return id;
}

uint32_t Scene::CreatePolylineEdge(uint32_t startPointId, uint32_t endPointId, const std::vector<uint32_t> &bridgePoints)
{
    edges.emplace_back();

    Edge &edge = edges.back();
    edge.startPointId = startPointId;
    edge.endPointId = endPointId;
    edge.bridgePoints = bridgePoints;
    edge.id = Id::GetId(Type::EDGE);

    uint32_t id = edge.id;
    uint32_t index = edges.size() - 1;
    edgeIdToIndex[id] = index;

    RegisterDependency(startPointId, id);
    RegisterDependency(endPointId, id);
    for (uint32_t pointId : bridgePoints)
        RegisterDependency(pointId, id);

    LOG_VOID("Created poly line edge");

    return id;
}

uint32_t Scene::CreateArcCurve(glm::dvec3 centerPosition, double radius)
{
    ArcData arc;
    arc.center = centerPosition;
    arc.radius = radius;

    curves.emplace_back();

    Curve &curve = curves.back();
    curve.data.operator=(arc);
    curve.id = Id::GetId(Type::CURVE);
    curve.type = CurveType::ARC;

    uint32_t id = curve.id;
    uint32_t index = curves.size() - 1;
    curveIdToIndex[id] = index;

    LOG_VOID("Created arc curve");

    return id;
}

uint32_t Scene::CreateNURBSCurve(const tinynurbs::RationalCurve3d &nurbs)
{
    curves.emplace_back();

    Curve &curve = curves.back();
    curve.data = std::make_unique<tinynurbs::RationalCurve3d>(nurbs);
    curve.id = Id::GetId(Type::CURVE);
    curve.type = CurveType::NURBS;

    uint32_t id = curve.id;
    uint32_t index = curves.size() - 1;
    curveIdToIndex[id] = index;

    LOG_VOID("Created nurbs curve");

    return id;
}

uint32_t Scene::CreatePlanarFace(const std::vector<std::vector<uint32_t>> &edgeLoops)
{
    faces.emplace_back();

    Face &face = faces.back();
    face.loops = edgeLoops;
    face.id = Id::GetId(Type::FACE);

    if (!edgeLoops.empty() && edgeLoops[0].size() >= 3)
    {
        const auto &outerLoop = edgeLoops[0];

        const Edge *e0 = GetEdge(outerLoop[0]);
        const Edge *e1 = GetEdge(outerLoop[1]);
        const Edge *e2 = GetEdge(outerLoop[2]);

        if (e0 && e1 && e2)
        {
            const Point *p0 = GetPoint(e0->startPointId);
            const Point *p1 = GetPoint(e0->endPointId);
            const Point *p2 = GetPoint(e1->endPointId);

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

    uint32_t id = face.id;
    uint32_t index = faces.size() - 1;
    faceIdToIndex[id] = index;

    for (std::vector<uint32_t> edgeLoop : edgeLoops)
    {
        for (uint32_t edgeId : edgeLoop)
            RegisterDependency(edgeId, id);
    }

    LOG_VOID("Created planar face");

    return id;
}

uint32_t Scene::CreateNURBSFace(const std::vector<std::vector<uint32_t>> &edgeLoops, const tinynurbs::RationalSurface3d &nurbs)
{
    faces.emplace_back();

    Face &face = faces.back();
    face.loops = edgeLoops;
    face.surface = std::make_unique<tinynurbs::RationalSurface3d>(nurbs);
    face.id = Id::GetId(Type::FACE);

    uint32_t id = face.id;
    uint32_t index = faces.size() - 1;
    faceIdToIndex[id] = index;

    for (std::vector<uint32_t> edgeLoop : edgeLoops)
    {
        for (uint32_t edgeId : edgeLoop)
            RegisterDependency(edgeId, id);
    }

    LOG_VOID("Created nurbs face");

    return id;
}

uint32_t Scene::CreateSolid(const std::vector<uint32_t> &faces)
{
    solids.emplace_back();

    Solid &solid = solids.back();
    solid.faceIds = faces;
    solid.id = Id::GetId(Type::SOLID);

    uint32_t id = solid.id;
    uint32_t index = solids.size() - 1;
    solidIdToIndex[id] = index;

    for (uint32_t faceId : faces)
        RegisterDependency(faceId, id);

    LOG_VOID("Created solid");

    return id;
}

const Point *Scene::GetPoint(uint32_t id) const
{
    auto hash = pointIdToIndex.find(id);
    if (hash == pointIdToIndex.end())
    {
        return nullptr;
    }
    return &points[hash->second];
}

const Edge *Scene::GetEdge(uint32_t id) const
{
    auto hash = edgeIdToIndex.find(id);
    if (hash == edgeIdToIndex.end())
        return nullptr;
    return &edges[hash->second];
}

const Curve *Scene::GetCurve(uint32_t id) const
{
    auto hash = curveIdToIndex.find(id);
    if (hash == curveIdToIndex.end())
    {
        return nullptr;
    }
    return &curves[hash->second];
}

Curve *Scene::GetCurve(uint32_t id)
{
    auto hash = curveIdToIndex.find(id);
    if (hash == curveIdToIndex.end())
    {
        return nullptr;
    }
    return &curves[hash->second];
}

const Face *Scene::GetFace(uint32_t id) const
{
    auto hash = faceIdToIndex.find(id);
    if (hash == faceIdToIndex.end())
    {
        return nullptr;
    }
    return &faces[hash->second];
}

Face *Scene::GetFace(uint32_t id)
{
    auto hash = faceIdToIndex.find(id);
    if (hash == faceIdToIndex.end())
    {
        return nullptr;
    }
    return &faces[hash->second];
}

const Solid *Scene::GetSolid(uint32_t id) const
{
    auto hash = solidIdToIndex.find(id);
    if (hash == solidIdToIndex.end())
    {
        return nullptr;
    }
    return &solids[hash->second];
}

void Scene::RegisterDependency(uint32_t formId, uint32_t dependancyId)
{
    fromIdToDependancies[formId].insert(dependancyId);
}

void Scene::UnregisterDependency(uint32_t formId, uint32_t dependancyId)
{
    auto hash = fromIdToDependancies.find(formId);
    std::unordered_set dependancy = hash->second;
    dependancy.erase(dependancyId);
}

void Scene::UnregisterDependency(uint32_t formId)
{
}

bool Scene::DeletePoint(uint32_t id)
{
    auto it = pointIdToIndex.find(id);
    if (it == pointIdToIndex.end())
        return LOG_FALSE("Point id not found: " + Log::NumToStr(id));

    uint32_t index = it->second;

    if (index != points.size() - 1)
    {
        std::swap(points[index], points.back());
        pointIdToIndex[points[index].id] = index;
    }

    points.pop_back();
    pointIdToIndex.erase(id);

    return true;
}

bool Scene::DeleteEdge(uint32_t id)
{
    auto it = edgeIdToIndex.find(id);
    if (it == edgeIdToIndex.end())
    {
        return LOG_FALSE("Edge id not found: " + Log::NumToStr(id));
    }

    uint32_t index = it->second;

    Edge &edge = edges[index];
    UnregisterDependency(edge.startPointId, id);
    UnregisterDependency(edge.endPointId, id);
    for (int i = 0; i < edge.bridgePoints.size(); i++)
        UnregisterDependency(edge.bridgePoints[i], id);

    if (index != edges.size() - 1)
    {
        std::swap(edges[index], edges.back());
        pointIdToIndex[edges[index].id] = index;
    }

    UnregisterDependency(id);
    edges.pop_back();
    edgeIdToIndex.erase(id);

    return true;
}

bool Scene::DeleteCurve(uint32_t id)
{
    auto it = curveIdToIndex.find(id);
    if (it == curveIdToIndex.end())
    {
        return LOG_FALSE("Curve id not found: " + Log::NumToStr(id));
    }

    uint32_t index = it->second;

    if (index != curves.size() - 1)
    {
        std::swap(curves[index], curves.back());
        pointIdToIndex[curves[index].id] = index;
    }

    UnregisterDependency(id);
    curves.pop_back();
    curveIdToIndex.erase(id);

    return true;
}

bool Scene::DeleteFace(uint32_t id)
{
    auto it = faceIdToIndex.find(id);
    if (it == faceIdToIndex.end())
    {
        return LOG_FALSE("Face id not found: " + Log::NumToStr(id));
    }

    uint32_t index = it->second;

    Face &face = faces[index];
    const auto &loops = face.GetLoops();
    const auto loopsSize = loops.size();
    for (int i = 0; i < loopsSize; i++)
    {
        int holeSize = loops[i].size();
        for (int k = 0; k < holeSize; i++)
        {
            UnregisterDependency(loops[i][k], id);
        }
    }

    UnregisterDependency(id);
    faces.pop_back();
    faceIdToIndex.erase(id);

    return true;
}

bool Scene::DeleteSolid(uint32_t id)
{
    auto it = solidIdToIndex.find(id);
    if (it == solidIdToIndex.end())
    {
        return LOG_FALSE("Solid id not found: " + Log::NumToStr(id));
    }

    uint32_t index = it->second;

    Solid &solid = solids[index];
    std::vector<uint32_t> &faces = solid.faceIds;
    for (uint32_t faceId : faces)
        UnregisterDependency(faceId, id);

    if (index != solids.size() - 1)
    {
        std::swap(solids[index], solids.back());
        pointIdToIndex[solids[index].id] = index;
    }

    UnregisterDependency(id);
    solids.pop_back();
    solidIdToIndex.erase(id);

    return true;
}