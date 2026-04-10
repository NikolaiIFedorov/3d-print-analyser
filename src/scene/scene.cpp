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

void Scene::MergeCoplanarFaces(Solid *solid)
{
    const double normalTolerance = 1e-3;

    // Debug: check edge dependency counts
    {
        int totalEdges = 0, multiDepEdges = 0;
        for (Face *f : solid->faces)
            for (const auto &loop : f->loops)
                for (const auto &oe : loop)
                {
                    totalEdges++;
                    if (oe.edge->dependencies.size() >= 2)
                        multiDepEdges++;
                }
        LOG_DEBU("Edges: " + std::to_string(totalEdges) + ", shared: " + std::to_string(multiDepEdges));
    }

    // Find an adjacent coplanar face to merge with
    auto findMergePair = [&](Face *fi) -> Face *
    {
        glm::dvec3 ni = glm::normalize(fi->GetSurface().GetNormal());
        for (const auto &loop : fi->loops)
            for (const auto &oe : loop)
                for (Face *candidate : oe.edge->dependencies)
                {
                    if (candidate == fi || candidate->dependency != solid)
                        continue;
                    glm::dvec3 nj = glm::normalize(candidate->GetSurface().GetNormal());
                    if (glm::dot(ni, nj) > 1.0 - normalTolerance)
                        return candidate;
                }
        return nullptr;
    };

    bool didMerge = true;
    while (didMerge)
    {
        didMerge = false;

        for (size_t i = 0; i < solid->faces.size(); i++)
        {
            Face *fi = solid->faces[i];
            Face *fj = findMergePair(fi);
            if (!fj)
                continue;

            // Collect edges from both faces and find shared ones
            std::unordered_set<Edge *> edgesI, edgesJ, shared;
            for (const auto &loop : fi->loops)
                for (const auto &oe : loop)
                    edgesI.insert(oe.edge);
            for (const auto &loop : fj->loops)
                for (const auto &oe : loop)
                    edgesJ.insert(oe.edge);
            for (Edge *e : edgesI)
                if (edgesJ.count(e))
                    shared.insert(e);

            // Non-shared edges form the merged boundary
            std::vector<Edge *> boundary;
            for (Edge *e : edgesI)
                if (!shared.count(e))
                    boundary.push_back(e);
            for (Edge *e : edgesJ)
                if (!shared.count(e))
                    boundary.push_back(e);

            // Chain boundary edges into loops
            std::unordered_map<Point *, std::vector<Edge *>> adj;
            for (Edge *e : boundary)
            {
                adj[e->startPoint].push_back(e);
                adj[e->endPoint].push_back(e);
            }

            std::unordered_set<Edge *> used;
            std::vector<std::vector<Edge *>> edgeLoops;

            for (Edge *startEdge : boundary)
            {
                if (used.count(startEdge))
                    continue;

                std::vector<Edge *> loop;
                used.insert(startEdge);
                loop.push_back(startEdge);

                Point *next = startEdge->endPoint;
                bool hasUnused = false;
                for (Edge *e : adj[next])
                    if (!used.count(e))
                    {
                        hasUnused = true;
                        break;
                    }
                if (!hasUnused)
                    next = startEdge->startPoint;

                while (true)
                {
                    bool found = false;
                    for (Edge *e : adj[next])
                    {
                        if (used.count(e))
                            continue;
                        used.insert(e);
                        loop.push_back(e);
                        next = (e->startPoint == next) ? e->endPoint : e->startPoint;
                        found = true;
                        break;
                    }
                    if (!found)
                        break;
                }
                edgeLoops.push_back(loop);
            }

            if (used.size() != boundary.size())
                continue;

            // Sort loops: largest first (outer boundary)
            std::sort(edgeLoops.begin(), edgeLoops.end(),
                      [](const auto &a, const auto &b)
                      { return a.size() > b.size(); });

            // Ensure consistent winding: outer loop CCW, inner loops CW
            // by checking signed area in the face's 2D projection
            glm::dvec3 origNormal = glm::normalize(fi->GetSurface().GetNormal());

            auto computeSignedArea = [&](const std::vector<Edge *> &loop) -> double
            {
                // Get ordered positions from the edge loop
                std::vector<glm::dvec3> positions;
                if (loop.empty())
                    return 0.0;

                Point *current = loop[0]->startPoint;
                // Check if first edge connects to second
                if (loop.size() > 1)
                {
                    Edge *e0 = loop[0];
                    Edge *e1 = loop[1];
                    if (e0->endPoint == e1->startPoint || e0->endPoint == e1->endPoint)
                        current = e0->startPoint;
                    else
                        current = e0->endPoint;
                }

                for (Edge *e : loop)
                {
                    positions.push_back(current->position);
                    current = (e->startPoint == current) ? e->endPoint : e->startPoint;
                }

                // Compute signed area projected along the face normal
                double area = 0.0;
                for (size_t j = 0; j < positions.size(); j++)
                {
                    size_t k = (j + 1) % positions.size();
                    glm::dvec3 cross = glm::cross(positions[j], positions[k]);
                    area += glm::dot(origNormal, cross);
                }
                return area;
            };

            for (size_t li = 0; li < edgeLoops.size(); li++)
            {
                double signedArea = computeSignedArea(edgeLoops[li]);
                bool shouldBeCCW = (li == 0); // outer loop should be CCW (positive area)
                if ((signedArea > 0) != shouldBeCCW)
                    std::reverse(edgeLoops[li].begin(), edgeLoops[li].end());
            }

            // Preserve original normal and plane equation
            double origD = glm::dot(origNormal, fi->loops[0][0].GetStartPosition());

            // Remove old faces from edge dependencies
            for (auto &loop : fi->loops)
                for (auto &oe : loop)
                    oe.edge->dependencies.erase(fi);
            fi->loops.clear();
            fi->dependency = nullptr;

            for (auto &loop : fj->loops)
                for (auto &oe : loop)
                    oe.edge->dependencies.erase(fj);
            fj->loops.clear();
            fj->dependency = nullptr;

            // Disconnect shared (internal) edges entirely
            for (Edge *e : shared)
            {
                e->startPoint->dependencies.erase(e);
                e->endPoint->dependencies.erase(e);
                e->startPoint = nullptr;
                e->endPoint = nullptr;
            }

            // Create merged face
            Face *merged = CreateFace(edgeLoops);
            merged->dependency = solid;

            auto &planar = static_cast<PlanarSurface *>(merged->surface.get())->data;
            planar.normal = origNormal;
            planar.d = origD;

            // Replace fi with merged, remove fj from solid
            solid->faces[i] = merged;
            auto it = std::find(solid->faces.begin(), solid->faces.end(), fj);
            if (it != solid->faces.end())
                solid->faces.erase(it);

            didMerge = true;
            break; // Restart from the beginning
        }
    }

    LOG_DEBU("Merged to " + std::to_string(solid->faces.size()) + " faces");
}