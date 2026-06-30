#include "Wireframe.hpp"
#include "rendering/SceneLighting.hpp"
#include "utils/utils.hpp"
#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <Poly_Polygon3D.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <TopLoc_Location.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS.hxx>

namespace
{
// Matches BRepMesh_IncrementalMesh linear deflection used during OCCT import.
constexpr double kOcctEdgeDisplayChordTolMm = 0.1;

// World-space chord deviation tolerance for analytic/NURBS curve tessellation.
// Camera-independent, so it never needs to be recomputed on zoom/orbit.
constexpr double kCurveChordToleranceMm = 0.1;
constexpr int kCurveTessellationMaxDepth = 7; // caps worst case at 4 * 2^7 = 512 segments
constexpr int kCurveTessellationBaseSegments = 4; // avoids missing curvature within one initial span

double PointToSegmentDistance(const glm::dvec3 &p, const glm::dvec3 &a, const glm::dvec3 &b)
{
    glm::dvec3 ab = b - a;
    double lenSq = glm::dot(ab, ab);
    if (lenSq < 1e-12)
        return glm::length(p - a);

    double t = glm::clamp(glm::dot(p - a, ab) / lenSq, 0.0, 1.0);
    glm::dvec3 closest = a + t * ab;
    return glm::length(p - closest);
}

void PushLineSegment(const gp_Pnt &p0,
                     const gp_Pnt &p1,
                     std::vector<Vertex> &vertices,
                     std::vector<uint32_t> &indices,
                     const glm::vec3 &color,
                     const glm::vec3 &litNormal)
{
    const uint32_t baseIndex = static_cast<uint32_t>(vertices.size());
    vertices.push_back({glm::vec3(static_cast<float>(p0.X()), static_cast<float>(p0.Y()), static_cast<float>(p0.Z())),
                        color, litNormal});
    vertices.push_back({glm::vec3(static_cast<float>(p1.X()), static_cast<float>(p1.Y()), static_cast<float>(p1.Z())),
                        color, litNormal});
    indices.push_back(baseIndex);
    indices.push_back(baseIndex + 1);
}

bool TryAddOcctEdgeFromFaceTriangulation(const Edge *edge,
                                         std::vector<Vertex> &vertices,
                                         std::vector<uint32_t> &indices,
                                         const glm::vec3 &color,
                                         const glm::vec3 &litNormal)
{
    if (edge == nullptr || edge->occtEdge.IsNull())
        return false;

    Handle(Poly_PolygonOnTriangulation) bestPolygon;
    Handle(Poly_Triangulation) bestTriangulation;
    TopLoc_Location bestLocation;
    int bestNodeCount = 0;

    auto considerPolygon = [&](const Handle(Poly_PolygonOnTriangulation) &polygon,
                               const Handle(Poly_Triangulation) &triangulation,
                               const TopLoc_Location &location)
    {
        if (polygon.IsNull() || triangulation.IsNull())
            return;
        const int nodeCount = polygon->NbNodes();
        if (nodeCount < 2 || nodeCount <= bestNodeCount)
            return;
        bestPolygon = polygon;
        bestTriangulation = triangulation;
        bestLocation = location;
        bestNodeCount = nodeCount;
    };

    auto tryEdgeOrientations = [&](const TopoDS_Edge &occtEdge,
                                   const Handle(Poly_Triangulation) &triangulation,
                                   const TopLoc_Location &location)
    {
        considerPolygon(BRep_Tool::PolygonOnTriangulation(occtEdge, triangulation, location),
                        triangulation,
                        location);
        considerPolygon(BRep_Tool::PolygonOnTriangulation(TopoDS::Edge(occtEdge.Reversed()), triangulation, location),
                        triangulation,
                        location);
    };

    for (Face *face : edge->dependencies)
    {
        if (face == nullptr || face->occtFace.IsNull())
            continue;
        TopLoc_Location faceLocation;
        Handle(Poly_Triangulation) triangulation = BRep_Tool::Triangulation(face->occtFace, faceLocation);
        if (triangulation.IsNull())
            continue;
        tryEdgeOrientations(edge->occtEdge, triangulation, faceLocation);
    }

    if (bestPolygon.IsNull())
    {
        Handle(Poly_PolygonOnTriangulation) polygon;
        Handle(Poly_Triangulation) triangulation;
        TopLoc_Location location;
        BRep_Tool::PolygonOnTriangulation(edge->occtEdge, polygon, triangulation, location);
        considerPolygon(polygon, triangulation, location);
        if (bestPolygon.IsNull())
        {
            BRep_Tool::PolygonOnTriangulation(TopoDS::Edge(edge->occtEdge.Reversed()),
                                              polygon,
                                              triangulation,
                                              location);
            considerPolygon(polygon, triangulation, location);
        }
    }

    if (bestPolygon.IsNull() || bestTriangulation.IsNull() || bestNodeCount < 2)
        return false;

    const int nbTriNodes = bestTriangulation->NbNodes();
    bool drewSegment = false;
    for (int i = 1; i < bestNodeCount; ++i)
    {
        const int idx0 = bestPolygon->Node(i);
        const int idx1 = bestPolygon->Node(i + 1);
        if (idx0 < 1 || idx0 > nbTriNodes || idx1 < 1 || idx1 > nbTriNodes)
            continue;
        gp_Pnt p0 = bestTriangulation->Node(idx0).Transformed(bestLocation);
        gp_Pnt p1 = bestTriangulation->Node(idx1).Transformed(bestLocation);
        PushLineSegment(p0, p1, vertices, indices, color, litNormal);
        drewSegment = true;
    }
    return drewSegment;
}

bool TryAddOcctEdgePolygon3D(const TopoDS_Edge &occtEdge,
                             std::vector<Vertex> &vertices,
                             std::vector<uint32_t> &indices,
                             const glm::vec3 &color,
                             const glm::vec3 &litNormal)
{
    TopLoc_Location loc;
    Handle(Poly_Polygon3D) polygon = BRep_Tool::Polygon3D(occtEdge, loc);
    if (polygon.IsNull())
        return false;

    const TColgp_Array1OfPnt &nodes = polygon->Nodes();
    if (nodes.Length() < 2)
        return false;

    for (int i = nodes.Lower(); i < nodes.Upper(); ++i)
    {
        gp_Pnt p0 = nodes(i).Transformed(loc);
        gp_Pnt p1 = nodes(i + 1).Transformed(loc);
        PushLineSegment(p0, p1, vertices, indices, color, litNormal);
    }
    return true;
}

bool TryAddOcctEdgeCurveTessellation(const TopoDS_Edge &occtEdge,
                                     std::vector<Vertex> &vertices,
                                     std::vector<uint32_t> &indices,
                                     const glm::vec3 &color,
                                     const glm::vec3 &litNormal)
{
    BRepAdaptor_Curve curve(occtEdge);
    if (curve.GetType() == GeomAbs_Line)
        return false;

    GCPnts_QuasiUniformDeflection disc(curve, kOcctEdgeDisplayChordTolMm);
    if (!disc.IsDone() || disc.NbPoints() < 2)
        return false;

    for (int i = 1; i < disc.NbPoints(); ++i)
        PushLineSegment(disc.Value(i), disc.Value(i + 1), vertices, indices, color, litNormal);
    return true;
}

glm::vec3 BrighterAdjacentFaceNormal(const Edge *edge, const glm::vec3 &lightDirUnit)
{
    if (edge == nullptr || edge->dependencies.empty())
        return glm::vec3(0.0f);

    const Face *bestFace = nullptr;
    float bestScore = -1.0f;
    for (Face *f : edge->dependencies)
    {
        if (f == nullptr || !f->HasGeometry())
            continue;
        glm::vec3 n = glm::vec3(f->GetSurface().GetNormal());
        const float len = glm::length(n);
        if (!(len > 1e-10f))
            continue;
        n /= len;
        const float score = glm::max(0.0f, glm::dot(n, lightDirUnit));
        if (score > bestScore)
        {
            bestScore = score;
            bestFace = f;
        }
    }
    if (bestFace == nullptr || !bestFace->HasGeometry())
        return glm::vec3(0.0f);
    glm::vec3 n = glm::vec3(bestFace->GetSurface().GetNormal());
    const float len = glm::length(n);
    if (!(len > 1e-10f))
        return glm::vec3(0.0f);
    return n / len;
}
} // namespace

void Wireframe::Generate(Scene *scene, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices, const AnalysisResults *results) const
{
    for (const Solid &solid : scene->solids)
        GenerateSolid(&solid, vertices, indices, results);

    GenerateLoose(scene, vertices, indices);
}

void Wireframe::GenerateSolid(const Solid *solid, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices,
                              const AnalysisResults *results) const
{
    AddSolid(solid, vertices, indices, results);
}

void Wireframe::GenerateLoose(Scene *scene, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices) const
{
    for (const Face &face : scene->faces)
    {
        if (!face.HasGeometry())
            continue;
        AddFace(&face, vertices, indices, false);
    }

    for (const Edge &edge : scene->edges)
    {
        if (edge.startPoint == nullptr || edge.endPoint == nullptr)
            continue;
        AddEdge(&edge, vertices, indices, false);
    }

    for (const Point &point : scene->points)
        AddPoint(&point, vertices, indices, false);
}

void Wireframe::AddPoint(const Point *point,
                         std::vector<Vertex> &vertices,
                         std::vector<uint32_t> &indices, bool isEdge) const
{
    // TODO: Add point
}

void Wireframe::AddEdge(const Edge *edge,
                        std::vector<Vertex> &vertices,
                        std::vector<uint32_t> &indices, bool isFace,
                        const glm::vec3 *colorOverride) const
{
    if (!edge->dependencies.empty() && !isFace)
        return;

    const glm::vec3 color = colorOverride ? *colorOverride : Color::GetEdge();
    const glm::vec3 litNormal =
        isFace ? BrighterAdjacentFaceNormal(edge, SceneLighting::DirectionalLightDirWorld()) : glm::vec3(0.0f);

    if (!edge->occtEdge.IsNull())
    {
        // Prefer the face-mesh edge polyline so wireframe tracks the same tessellation as Patch.
        if (TryAddOcctEdgeFromFaceTriangulation(edge, vertices, indices, color, litNormal))
            return;
        if (TryAddOcctEdgePolygon3D(edge->occtEdge, vertices, indices, color, litNormal))
            return;
        if (TryAddOcctEdgeCurveTessellation(edge->occtEdge, vertices, indices, color, litNormal))
            return;
    }

    if (edge->curve == nullptr)
        AddLineEdge(edge, vertices, indices, color, litNormal);
    else
        AddCurvedEdge(edge, vertices, indices, color, litNormal);
}

void Wireframe::AddLineEdge(const Edge *edge,
                            std::vector<Vertex> &vertices,
                            std::vector<uint32_t> &indices,
                            const glm::vec3 &color,
                            const glm::vec3 &litNormal) const
{
    const Point *p0 = edge->startPoint;
    const Point *p1 = edge->endPoint;

    uint32_t baseIndex = vertices.size();

    Vertex v0, v1;
    v0.position = glm::vec3(p0->position);
    v0.color = color;
    v0.normal = litNormal;

    v1.position = glm::vec3(p1->position);
    v1.color = color;
    v1.normal = litNormal;

    vertices.push_back(v0);
    vertices.push_back(v1);

    indices.push_back(baseIndex);
    indices.push_back(baseIndex + 1);
}

void Wireframe::AddCurvedEdge(const Edge *edge,
                              std::vector<Vertex> &vertices,
                              std::vector<uint32_t> &indices,
                              const glm::vec3 &color,
                              const glm::vec3 &litNormal) const
{
    const Point *p0 = edge->startPoint;
    if (p0 == nullptr)
        return;

    const Point *p1 = edge->endPoint;
    if (p1 == nullptr)
        return;

    TessellateCurve(edge->curve, p0->position, p1->position, vertices, indices, color, litNormal);
}

void Wireframe::AddFace(const Face *face,
                        std::vector<Vertex> &vertices,
                        std::vector<uint32_t> &indices, bool isSolid) const
{
    if (face->dependency != nullptr && !isSolid)
        return;
    if (!face->HasGeometry())
        return;

    for (const auto &loop : face->loops)
    {
        for (const auto &orientedEdge : loop)
            AddEdge(orientedEdge.edge, vertices, indices, true);
    }
}

void Wireframe::AddSolid(const Solid *solid,
                         std::vector<Vertex> &vertices,
                         std::vector<uint32_t> &indices, const AnalysisResults *results) const
{
    (void)results; // no detector produces edge-level flaws under the current Analysis design
    for (auto face : solid->faces)
        AddFace(face, vertices, indices, true);
}

void Wireframe::TessellateCurveRecursive(const Curve *curve,
                                         const glm::dvec3 &start,
                                         const glm::dvec3 &end,
                                         double t0, double t1,
                                         const glm::dvec3 &p0, const glm::dvec3 &p1,
                                         int depth,
                                         std::vector<Vertex> &vertices,
                                         std::vector<uint32_t> &indices,
                                         const glm::vec3 &color,
                                         const glm::vec3 &litNormal) const
{
    double tm = 0.5 * (t0 + t1);
    glm::dvec3 pm = curve->Evaluate(tm, start, end);

    if (depth >= kCurveTessellationMaxDepth ||
        PointToSegmentDistance(pm, p0, p1) <= kCurveChordToleranceMm)
    {
        uint32_t baseIndex = static_cast<uint32_t>(vertices.size());
        vertices.push_back({glm::vec3(p0), color, litNormal});
        vertices.push_back({glm::vec3(p1), color, litNormal});
        indices.push_back(baseIndex);
        indices.push_back(baseIndex + 1);
        return;
    }

    TessellateCurveRecursive(curve, start, end, t0, tm, p0, pm, depth + 1, vertices, indices, color, litNormal);
    TessellateCurveRecursive(curve, start, end, tm, t1, pm, p1, depth + 1, vertices, indices, color, litNormal);
}

void Wireframe::TessellateCurve(const Curve *curve,
                                const glm::dvec3 &start,
                                const glm::dvec3 &end,
                                std::vector<Vertex> &vertices,
                                std::vector<uint32_t> &indices,
                                const glm::vec3 &color,
                                const glm::vec3 &litNormal) const
{
    for (int i = 0; i < kCurveTessellationBaseSegments; i += 1)
    {
        double t0 = (double)i / kCurveTessellationBaseSegments;
        double t1 = (double)(i + 1) / kCurveTessellationBaseSegments;

        glm::dvec3 p0 = curve->Evaluate(t0, start, end);
        glm::dvec3 p1 = curve->Evaluate(t1, start, end);

        TessellateCurveRecursive(curve, start, end, t0, t1, p0, p1, 0, vertices, indices, color, litNormal);
    }
}
