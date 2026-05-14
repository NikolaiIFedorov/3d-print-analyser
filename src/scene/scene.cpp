#include "scene.hpp"
#include "GeometryExperiments.hpp"
#include "GeometryValidity.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static double MergePlaneTolFromDiagonal(double diagonal)
{
    const double planeTolGeometric = std::clamp(1e-7 * diagonal, 1e-10, 1.0);
    const double planeTolFloat = 4.0 * static_cast<double>(FLT_EPSILON) * std::max(diagonal, 1e-300);
    const double planeTolImport = 2e-6 * std::max(diagonal, 1e-300);
    return std::max({planeTolGeometric, planeTolFloat, planeTolImport,
                     GeometryExperiments::kMergeCoplanarPlaneTolFloor});
}

namespace
{
// Import and rebuild can create many entities; keep scene-construction logs off by default.
inline constexpr bool kLogSceneConstruction = false;
inline constexpr bool kLogMergeDebug = false;

static std::unordered_set<Point *> CollectFaceBoundaryPoints(Face *f)
{
    std::unordered_set<Point *> pts;
    if (!f)
        return pts;
    for (const auto &loop : f->loops)
        for (const auto &oe : loop)
        {
            if (oe.edge == nullptr)
                continue;
            if (Point *a = oe.GetStart())
                pts.insert(a);
            if (Point *b = oe.GetEnd())
                pts.insert(b);
        }
    return pts;
}

/// Corner-to-corner diagonal of vertices used by faces in `solid`; used by coplanar merge tolerances.
static double SolidDiagonalFromFacePoints(const Solid *solid)
{
    glm::dvec3 boundMin(std::numeric_limits<double>::max());
    glm::dvec3 boundMax(std::numeric_limits<double>::lowest());
    bool anyBounds = false;
    if (!solid)
        return 0.0;

    for (Face *f : solid->faces)
    {
        for (Point *p : CollectFaceBoundaryPoints(f))
        {
            if (p == nullptr)
                continue;
            anyBounds = true;
            boundMin = glm::min(boundMin, p->position);
            boundMax = glm::max(boundMax, p->position);
        }
    }

    return anyBounds ? glm::length(boundMax - boundMin) : 0.0;
}

static void EdgeSharingHistogramSolid(const Solid *solid,
                                      std::size_t &edgesOneFace,
                                      std::size_t &edgesTwoFaces,
                                      std::size_t &edgesThreePlus)
{
    edgesOneFace = edgesTwoFaces = edgesThreePlus = 0;
    if (!solid)
        return;

    std::unordered_set<Edge *> seen;
    for (Face *f : solid->faces)
    {
        if (!f)
            continue;
        for (const auto &loop : f->loops)
            for (const auto &oe : loop)
            {
                Edge *e = oe.edge;
                if (e == nullptr || e->startPoint == nullptr || e->endPoint == nullptr)
                    continue;
                seen.insert(e);
            }
    }

    for (Edge *e : seen)
    {
        const std::size_t nf = e->dependencies.size();
        if (nf <= 1)
            edgesOneFace++;
        else if (nf == 2)
            edgesTwoFaces++;
        else
            edgesThreePlus++;
    }
}

static void PopulateMergeDiagnosticSnapshot(Solid *solid, MergeCoplanarDiagnostics &d,
                                            bool setFaceCountsMatchingBefore)
{
    d.facesBefore = solid->faces.size();
    if (setFaceCountsMatchingBefore)
        d.facesAfter = d.facesBefore;
    d.bboxDiagonal = SolidDiagonalFromFacePoints(solid);
    d.planeTolUsed = MergePlaneTolFromDiagonal(d.bboxDiagonal);
    EdgeSharingHistogramSolid(solid, d.edgesOneFaceBefore, d.edgesTwoFacesBefore, d.edgesThreePlusBefore);
}

} // namespace

Point *Scene::CreatePoint(const glm::dvec3 &position)
{
    points.emplace_back(position);

    if constexpr (kLogSceneConstruction)
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

    if constexpr (kLogSceneConstruction)
        LOG_VOID("Created line edge");

    return &edge;
}

Edge *Scene::CreateEdge(Point *startPoint, Point *endPoint, Curve *curve)
{
    Edge *edge = CreateEdge(startPoint, endPoint);

    edge->curve = curve;
    curve->dependencies.insert(edge);

    if constexpr (kLogSceneConstruction)
        LOG_VOID("Created line edge");

    return edge;
}

Edge *Scene::CreateEdge(Point *startPoint, Point *endPoint, const std::vector<Point *> &bridgePoints)
{
    Edge *edge = CreateEdge(startPoint, endPoint);

    edge->bridgePoints = bridgePoints;
    for (Point *point : bridgePoints)
        point->dependencies.insert(edge);

    if constexpr (kLogSceneConstruction)
        LOG_VOID("Created poly line edge");

    return edge;
}

Curve *Scene::CreateCurve(glm::dvec3 centerPosition, double radius)
{
    ArcData arc;
    arc.center = centerPosition;
    arc.radius = radius;

    curves.push_back(std::make_unique<ArcCurve>(arc));

    if constexpr (kLogSceneConstruction)
        LOG_VOID("Created arc curve");

    return curves.back().get();
}

Curve *Scene::CreateCurve(const tinynurbs::RationalCurve3d &nurbs)
{
    curves.push_back(std::make_unique<NurbsCurve>(
        std::make_unique<tinynurbs::RationalCurve3d>(nurbs)));

    if constexpr (kLogSceneConstruction)
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

    if constexpr (kLogSceneConstruction)
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

    if constexpr (kLogSceneConstruction)
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

    GeometryValidity::RefreshSolidAppGeometryValidityCache(solid);

    if constexpr (kLogSceneConstruction)
        LOG_VOID("Created solid");

    return &solid;
}

std::unique_ptr<Scene> Scene::Clone(std::unordered_map<const Face *, Face *> *outFaceRemap) const
{
    auto out = std::make_unique<Scene>();

    std::unordered_map<const Point *, Point *> pointMap;
    pointMap.reserve(points.size());
    for (const Point &p : points)
        pointMap.emplace(&p, out->CreatePoint(p.position));

    std::unordered_map<const Curve *, Curve *> curveMap;
    curveMap.reserve(curves.size());
    for (const std::unique_ptr<Curve> &cu : curves)
    {
        Curve *nc = nullptr;
        if (const auto *arc = dynamic_cast<const ArcCurve *>(cu.get()))
            nc = out->CreateCurve(arc->arc.center, arc->arc.radius);
        else if (const auto *nurbs = dynamic_cast<const NurbsCurve *>(cu.get()))
            nc = out->CreateCurve(*nurbs->nurbs);
        if (nc != nullptr)
            curveMap.emplace(cu.get(), nc);
    }

    std::unordered_map<const Edge *, Edge *> edgeMap;
    edgeMap.reserve(edges.size());
    for (const Edge &e : edges)
    {
        auto itStart = pointMap.find(e.startPoint);
        auto itEnd = pointMap.find(e.endPoint);
        if (itStart == pointMap.end() || itEnd == pointMap.end())
            continue;
        Edge *ne = nullptr;
        if (e.curve != nullptr)
        {
            auto itCurve = curveMap.find(e.curve);
            Curve *nc = (itCurve != curveMap.end()) ? itCurve->second : nullptr;
            ne = nc ? out->CreateEdge(itStart->second, itEnd->second, nc)
                    : out->CreateEdge(itStart->second, itEnd->second);
        }
        else if (!e.bridgePoints.empty())
        {
            std::vector<Point *> bridge;
            bridge.reserve(e.bridgePoints.size());
            for (Point *bp : e.bridgePoints)
            {
                auto it = pointMap.find(bp);
                if (it != pointMap.end())
                    bridge.push_back(it->second);
            }
            ne = out->CreateEdge(itStart->second, itEnd->second, bridge);
        }
        else
        {
            ne = out->CreateEdge(itStart->second, itEnd->second);
        }
        if (ne != nullptr)
            edgeMap.emplace(&e, ne);
    }

    std::unordered_map<const Face *, Face *> faceMap;
    faceMap.reserve(faces.size());
    for (const Face &f : faces)
    {
        std::vector<std::vector<Edge *>> newLoops;
        newLoops.reserve(f.loops.size());
        bool loopsValid = true;
        for (const auto &loop : f.loops)
        {
            std::vector<Edge *> nl;
            nl.reserve(loop.size());
            for (const OrientedEdge &oe : loop)
            {
                auto it = edgeMap.find(oe.edge);
                if (it == edgeMap.end())
                {
                    loopsValid = false;
                    break;
                }
                nl.push_back(it->second);
            }
            if (!loopsValid)
                break;
            newLoops.push_back(std::move(nl));
        }
        if (!loopsValid || newLoops.empty())
            continue;
        Face *nf = nullptr;
        if (const auto *ns = dynamic_cast<const NurbsSurface *>(f.surface.get()))
            nf = out->CreateFace(newLoops, *ns->nurbs);
        else
            nf = out->CreateFace(newLoops);
        if (nf != nullptr)
            faceMap.emplace(&f, nf);
    }

    for (const Solid &s : solids)
    {
        std::vector<Face *> newFaces;
        newFaces.reserve(s.faces.size());
        for (Face *of : s.faces)
        {
            auto it = faceMap.find(of);
            if (it != faceMap.end())
                newFaces.push_back(it->second);
        }
        out->CreateSolid(newFaces);
    }

    out->renderBuffer = renderBuffer;
    out->lockedBuffer = lockedBuffer;

    if (outFaceRemap != nullptr)
    {
        outFaceRemap->clear();
        outFaceRemap->reserve(faceMap.size());
        outFaceRemap->insert(faceMap.begin(), faceMap.end());
    }

    return out;
}

void Scene::CompleteCoplanarMergeDiagnostics(Solid *solid, MergeCoplanarDiagnostics *diagnosticsOut)
{
    if (!solid || !diagnosticsOut)
        return;
    diagnosticsOut->facesAfter = solid->faces.size();
    EdgeSharingHistogramSolid(solid,
                                  diagnosticsOut->edgesOneFaceAfter,
                                  diagnosticsOut->edgesTwoFacesAfter,
                                  diagnosticsOut->edgesThreePlusAfter);
}

void Scene::RecordCoplanarDiagnosticsBaselineForMerge(Solid *solid, MergeCoplanarDiagnostics *diag)
{
    if (!solid || !diag)
        return;
    diag->mergeRan = true;
    diag->mergeWhileIterations = 0;
    diag->mergeOperations = 0;
    diag->boundaryLoopFailures = 0;
    PopulateMergeDiagnosticSnapshot(solid, *diag, false);
}

void Scene::CollectCoplanarMergeTopology(Solid *solid, MergeCoplanarDiagnostics *out)
{
    if (!solid || !out)
        return;

    out->mergeRan = false;
    PopulateMergeDiagnosticSnapshot(solid, *out, true);
    out->edgesOneFaceAfter = out->edgesOneFaceBefore;
    out->edgesTwoFacesAfter = out->edgesTwoFacesBefore;
    out->edgesThreePlusAfter = out->edgesThreePlusBefore;
}

void Scene::MergeCoplanarFaces(
    Solid *solid,
    MergeCoplanarDiagnostics *diagnosticsOut,
    const SceneProgressCallback *progress)
{
    if (!solid)
        return;

    const double normalTolerance = GeometryExperiments::kMergeCoplanarNormalDotSlack;

    auto maxSignedPlaneDistance = [](Face *f, const glm::dvec3 &unitN, double d) -> double
    {
        double m = 0.0;
        for (Point *p : CollectFaceBoundaryPoints(f))
        {
            if (p == nullptr)
                continue;
            m = std::max(m, std::abs(glm::dot(unitN, p->position) - d));
        }
        return m;
    };

    MergeCoplanarDiagnostics *diag = diagnosticsOut;
    if (diag)
        RecordCoplanarDiagnosticsBaselineForMerge(solid, diag);

    const double diagonal = diag ? diag->bboxDiagonal : SolidDiagonalFromFacePoints(solid);
    const double planeTol = MergePlaneTolFromDiagonal(diagonal);
    const std::size_t initialFaceCount = solid->faces.size();
    float lastReportedProgress = -1.0f;
    auto reportProgress = [&](float progress01, bool force = false)
    {
        if (progress == nullptr || !*progress)
            return;
        progress01 = std::clamp(progress01, 0.0f, 1.0f);
        if (!force && lastReportedProgress >= 0.0f && progress01 - lastReportedProgress < 0.005f)
            return;
        lastReportedProgress = progress01;
        (*progress)(progress01);
    };
    reportProgress(0.0f, true);

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
        if constexpr (kLogMergeDebug)
            LOG_DEBU("Edges: " + std::to_string(totalEdges) + ", shared: " + std::to_string(multiDepEdges));
    }

    // All coplanar solid neighbors across shared edges (order not guaranteed).
    auto collectCoplanarNeighbors = [&](Face *fi) -> std::vector<Face *>
    {
        std::vector<Face *> out;
        std::unordered_set<Face *> seen;
        if (!fi->GetSurface().IsPlanar())
            return out;
        const PlanarData &di = static_cast<const PlanarSurface *>(&fi->GetSurface())->data;
        const glm::dvec3 ni = glm::normalize(di.normal);

        for (const auto &loop : fi->loops)
            for (const auto &oe : loop)
                for (Face *candidate : oe.edge->dependencies)
                {
                    if (candidate == fi || candidate->dependency != solid)
                        continue;
                    if (!candidate->GetSurface().IsPlanar())
                        continue;
                    const PlanarData &dj = static_cast<const PlanarSurface *>(&candidate->GetSurface())->data;
                    const glm::dvec3 nj = glm::normalize(dj.normal);
                    if (std::abs(glm::dot(ni, nj)) < 1.0 - normalTolerance)
                        continue;
                    if (maxSignedPlaneDistance(candidate, ni, di.d) > planeTol)
                        continue;
                    if (maxSignedPlaneDistance(fi, ni, di.d) > planeTol)
                        continue;
                    if (seen.insert(candidate).second)
                        out.push_back(candidate);
                }
        return out;
    };

    bool didMerge = true;
    while (didMerge)
    {
        didMerge = false;

        if (diag)
            diag->mergeWhileIterations++;

        for (size_t i = 0; i < solid->faces.size(); i++)
        {
            Face *fi = solid->faces[i];
            const std::vector<Face *> neighbors = collectCoplanarNeighbors(fi);
            bool mergedThisFi = false;

            for (Face *fj : neighbors)
            {
                if (fj == nullptr)
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
            {
                if (diag)
                    diag->boundaryLoopFailures++;
                continue; // try another coplanar neighbor of fi
            }

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

            // Sort loops: largest absolute area first (outer boundary)
            std::sort(edgeLoops.begin(), edgeLoops.end(),
                      [&](const auto &a, const auto &b)
                      { return std::abs(computeSignedArea(a)) > std::abs(computeSignedArea(b)); });

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
            mergedThisFi = true;
            if (diag)
                diag->mergeOperations++;
            if (initialFaceCount > 1)
            {
                const std::size_t removedFaces = initialFaceCount - solid->faces.size();
                reportProgress(static_cast<float>(removedFaces) / static_cast<float>(initialFaceCount - 1));
            }
            break; // done with neighbors of fi; outer `while` will rescan
            }

            if (mergedThisFi)
                break; // restart for-loop over faces from index 0
        }
    }

    if (diag)
        CompleteCoplanarMergeDiagnostics(solid, diag);
    reportProgress(1.0f, true);

    GeometryValidity::RefreshSolidAppGeometryValidityCache(*solid);

    if constexpr (kLogMergeDebug)
        LOG_DEBU("Merged to " + std::to_string(solid->faces.size()) + " faces");
}