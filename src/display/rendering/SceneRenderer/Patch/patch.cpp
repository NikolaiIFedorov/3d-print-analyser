#include "patch.hpp"
#include "RenderingExperiments.hpp"
#include "utils/log.hpp"
#include <BRep_Tool.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepGProp.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GProp_GProps.hxx>
#include <Poly_Triangulation.hxx>
#include <TopLoc_Location.hxx>

static void CreatePlaneCoordinateSystem(const glm::dvec3 &normal,
                                        glm::dvec3 &uAxis,
                                        glm::dvec3 &vAxis)
{
    glm::dvec3 arbitrary;
    if (std::abs(normal.x) < 0.9)
    {
        arbitrary = glm::dvec3(1, 0, 0);
    }
    else
    {
        arbitrary = glm::dvec3(0, 1, 0);
    }

    uAxis = glm::normalize(glm::cross(normal, arbitrary));
    vAxis = glm::normalize(glm::cross(normal, uAxis));
}

static glm::dvec2 ProjectPointToPlane(const glm::dvec3 &point3D,
                                      const glm::dvec3 &origin,
                                      const glm::dvec3 &uAxis,
                                      const glm::dvec3 &vAxis)
{
    glm::dvec3 relativePos = point3D - origin;
    return glm::dvec2(
        glm::dot(relativePos, uAxis),
        glm::dot(relativePos, vAxis));
}

void Patch::Generate(Scene *scene, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices, int viewport[4],
                     const AnalysisResults *results, std::vector<PickTriangle> *pickOut) const
{
    for (const Solid &solid : scene->solids)
        GenerateSolid(&solid, vertices, indices, viewport, results, pickOut);

    GenerateLoose(scene, vertices, indices, viewport, results, pickOut);
}

void Patch::GenerateSolid(const Solid *solid, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices,
                          int viewport[4], const AnalysisResults *results, std::vector<PickTriangle> *pickOut) const
{
    (void)viewport;
    AddSolid(solid, vertices, indices, results, pickOut);
}

void Patch::GenerateLoose(Scene *scene, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices,
                          int viewport[4], const AnalysisResults *results, std::vector<PickTriangle> *pickOut) const
{
    (void)viewport;
    for (const Face &face : scene->faces)
        AddFace(&face, vertices, indices, false, results, pickOut);
}

void Patch::AddFace(const Face *face,
                    std::vector<Vertex> &vertices,
                    std::vector<uint32_t> &indices, bool isSolid, const AnalysisResults *results,
                    std::vector<PickTriangle> *pickOut) const
{
    if (face->dependency != nullptr && !isSolid)
        return;
    // Orphaned / tombstoned faces (e.g. after OCCT rebuild) must not draw or dereference surface.
    if (!face->HasGeometry())
    {
        if (face->dependency == nullptr)
            return;
        return;
    }

    glm::vec3 faceColor = Color::GetFace();
    if (results)
    {
        for (const auto &[solid, faceFlawList] : results->faceFlawRanges)
        {
            for (const auto &ff : faceFlawList)
            {
                if (ff.face == face && ff.flaw != FaceFlawKind::NOT_ENOUGH_SPACE && ff.flaw != FaceFlawKind::SHARP_CORNER)
                {
                    faceColor = glm::vec3(Color::GetFace(ff.flaw));
                    goto colorResolved;
                }
            }
        }
    colorResolved:;
    }

    if (!face->occtFace.IsNull())
    {
        TopLoc_Location loc;
        Handle(Poly_Triangulation) triangulation = BRep_Tool::Triangulation(face->occtFace, loc);
        if (!triangulation.IsNull())
        {
            uint32_t baseVertexIndex = vertices.size();

            GProp_GProps massProps;
            BRepGProp::SurfaceProperties(face->occtFace, massProps);
            const float faceSize = static_cast<float>(std::sqrt(std::max(massProps.Mass(), 0.0)));

            // Extract nodes
            std::vector<glm::dvec3> flatPositions;
            flatPositions.reserve(triangulation->NbNodes());
            for (int i = 1; i <= triangulation->NbNodes(); ++i)
            {
                gp_Pnt p = triangulation->Node(i).Transformed(loc);
                flatPositions.push_back(glm::dvec3(p.X(), p.Y(), p.Z()));
            }

            // Extract normals if available, else fallback
            bool hasNormals = triangulation->HasNormals();
            for (int i = 1; i <= triangulation->NbNodes(); ++i)
            {
                Vertex v;
                v.position = glm::vec3(flatPositions[i - 1]);
                v.color = faceColor;
                v.faceSize = faceSize;
                if (hasNormals) {
                    gp_Dir n = triangulation->Normal(i);
                    if (face->occtFace.Orientation() == TopAbs_REVERSED) {
                        n.Reverse();
                    }
                    v.normal = glm::vec3(n.X(), n.Y(), n.Z());
                } else {
                    v.normal = glm::vec3(face->GetSurface().GetNormal());
                }
                vertices.push_back(v);
            }

            // Extract triangles
            const int nbNodes = triangulation->NbNodes();
            for (int i = 1; i <= triangulation->NbTriangles(); ++i)
            {
                const Poly_Triangle& tri = triangulation->Triangle(i);
                int n1, n2, n3;
                tri.Get(n1, n2, n3);

                if (face->occtFace.Orientation() == TopAbs_REVERSED) {
                    std::swap(n1, n3);
                }

                if (n1 < 1 || n1 > nbNodes || n2 < 1 || n2 > nbNodes || n3 < 1 || n3 > nbNodes)
                    continue;

                uint32_t ia = static_cast<uint32_t>(n1 - 1);
                uint32_t ib = static_cast<uint32_t>(n2 - 1);
                uint32_t ic = static_cast<uint32_t>(n3 - 1);

                if (RenderingExperiments::kCullDegeneratePatchTriangles)
                {
                    const glm::dvec3 &p0 = flatPositions[ia];
                    const glm::dvec3 &p1 = flatPositions[ib];
                    const glm::dvec3 &p2 = flatPositions[ic];
                    const glm::dvec3 e1 = p1 - p0;
                    const glm::dvec3 e2 = p2 - p0;
                    const double crossLen = glm::length(glm::cross(e1, e2));
                    if (crossLen < RenderingExperiments::kDegeneratePatchMinCrossLen)
                        continue;
                }

                indices.push_back(baseVertexIndex + ia);
                indices.push_back(baseVertexIndex + ib);
                indices.push_back(baseVertexIndex + ic);

                if (pickOut != nullptr)
                    pickOut->push_back(PickTriangle{face, flatPositions[ia], flatPositions[ib], flatPositions[ic]});
            }
            return;
        }
        // occtFace set but not meshed yet (e.g. post-boolean rebuild) — fan from loops below.
    }

    glm::dvec3 faceNormal = face->GetSurface().GetNormal();

    glm::dvec3 uAxis, vAxis;
    CreatePlaneCoordinateSystem(faceNormal, uAxis, vAxis);

    using Point2D = std::array<double, 2>;
    std::vector<std::vector<Point2D>> polygon;
    std::vector<std::vector<glm::dvec3>> allLoopPositions;

    glm::dvec3 projectionOrigin(0, 0, 0);
    bool originSet = false;

    for (const auto &edgeLoop : face->loops)
    {
        std::vector<glm::dvec3> loopPositions;

        for (const auto &orientedEdge : edgeLoop)
        {
            if (orientedEdge.edge == nullptr)
                continue;
            // Edges are already oriented correctly by Face constructor
            const Point *p0 = orientedEdge.GetStart();
            if (p0 == nullptr)
                continue;
            const Point *p1 = orientedEdge.GetEnd();

            // Set projection origin from first point
            if (!originSet)
            {
                projectionOrigin = p0->position;
                originSet = true;
            }

            if (orientedEdge.edge->curve == nullptr)
            {
                // For OCCT-backed curved edges (no scene Curve object): tessellate the
                // underlying OCCT edge geometry at the same chord tolerance the wireframe
                // uses, so the earcut boundary matches the wireframe exactly.
                bool occtCurvePushed = false;
                if (!orientedEdge.edge->occtEdge.IsNull())
                {
                    BRepAdaptor_Curve adaptor(orientedEdge.edge->occtEdge);
                    if (adaptor.GetType() != GeomAbs_Line)
                    {
                        constexpr double kEarCutChordTolMm = 0.1;
                        GCPnts_QuasiUniformDeflection disc(adaptor, kEarCutChordTolMm);
                        if (disc.IsDone() && disc.NbPoints() >= 2)
                        {
                            std::vector<glm::dvec3> pts;
                            pts.reserve(static_cast<size_t>(disc.NbPoints()));
                            for (int k = 1; k <= disc.NbPoints(); ++k)
                            {
                                const gp_Pnt gp = disc.Value(k);
                                pts.push_back(glm::dvec3(gp.X(), gp.Y(), gp.Z()));
                            }
                            // Align orientation with the oriented edge (p0 is the start).
                            if (p0 != nullptr && !pts.empty())
                            {
                                const glm::dvec3 &front = pts.front();
                                const glm::dvec3 &back = pts.back();
                                const glm::dvec3 &start = p0->position;
                                if (glm::length(back - start) < glm::length(front - start))
                                    std::reverse(pts.begin(), pts.end());
                            }
                            // Push all but the last (next edge's start == this edge's end).
                            for (size_t k = 0; k + 1 < pts.size(); ++k)
                                loopPositions.push_back(pts[k]);
                            occtCurvePushed = true;
                        }
                    }
                }
                if (!occtCurvePushed)
                    loopPositions.push_back(p0->position);
            }
            else
            {
                const Curve *curve = orientedEdge.edge->curve;

                std::vector<glm::dvec3> tessellatedPoints = TessellateCurveToPoints(
                    curve,
                    orientedEdge.GetStartPosition(),
                    orientedEdge.GetEndPosition(),
                    16);

                for (size_t j = 0; j < tessellatedPoints.size() - 1; j++)
                {
                    loopPositions.push_back(tessellatedPoints[j]);
                }
            }
        }

        allLoopPositions.push_back(loopPositions);

        std::vector<Point2D> loop2D;
        for (const glm::dvec3 &pos : loopPositions)
        {
            glm::dvec2 pos2D = ProjectPointToPlane(pos, projectionOrigin, uAxis, vAxis);
            loop2D.push_back({pos2D.x, pos2D.y});
        }
        polygon.push_back(loop2D);
    }

    if (polygon.empty())
        return;

    double outerLoopArea = 0.0;

    // Ensure consistent winding for earcut: outer loop CCW, inner loops CW
    {
        auto signedArea2D = [](const std::vector<Point2D> &loop) -> double
        {
            double area = 0.0;
            for (size_t j = 0; j < loop.size(); j++)
            {
                size_t k = (j + 1) % loop.size();
                area += loop[j][0] * loop[k][1] - loop[k][0] * loop[j][1];
            }
            return area;
        };

        outerLoopArea = std::abs(signedArea2D(polygon[0])) * 0.5;

        // Outer loop (index 0) should be CCW (positive signed area)
        if (signedArea2D(polygon[0]) < 0)
        {
            std::reverse(polygon[0].begin(), polygon[0].end());
            std::reverse(allLoopPositions[0].begin(), allLoopPositions[0].end());
        }

        // Inner loops should be CW (negative signed area)
        for (size_t li = 1; li < polygon.size(); li++)
        {
            if (signedArea2D(polygon[li]) > 0)
            {
                std::reverse(polygon[li].begin(), polygon[li].end());
                std::reverse(allLoopPositions[li].begin(), allLoopPositions[li].end());
            }
        }
    }

    std::vector<glm::dvec3> flatPositions;
    for (const auto &loopPositions : allLoopPositions)
    {
        for (const glm::dvec3 &pos : loopPositions)
            flatPositions.push_back(pos);
    }

    std::vector<uint32_t> triangleIndices = mapbox::earcut<uint32_t>(polygon);

    if (triangleIndices.empty())
    {
        std::string dbg = "Earcut failed: " + std::to_string(polygon.size()) + " loops, sizes:";
        for (const auto &loop : polygon)
            dbg += " " + std::to_string(loop.size());
        LOG_WARN(dbg);
        return;
    }

    uint32_t baseVertexIndex = vertices.size();
    const float faceSize = static_cast<float>(std::sqrt(outerLoopArea));

    for (const auto &loopPositions : allLoopPositions)
    {
        for (const glm::dvec3 &pos : loopPositions)
        {
            Vertex v;
            v.position = glm::vec3(pos);
            v.color = faceColor;
            v.normal = glm::vec3(faceNormal);
            v.faceSize = faceSize;

            vertices.push_back(v);
        }
    }

    for (size_t i = 0; i + 2 < triangleIndices.size(); i += 3)
    {
        const uint32_t ia = triangleIndices[i];
        const uint32_t ib = triangleIndices[i + 1];
        const uint32_t ic = triangleIndices[i + 2];

        if (RenderingExperiments::kCullDegeneratePatchTriangles)
        {
            const glm::dvec3 &p0 = flatPositions[ia];
            const glm::dvec3 &p1 = flatPositions[ib];
            const glm::dvec3 &p2 = flatPositions[ic];
            const glm::dvec3 e1 = p1 - p0;
            const glm::dvec3 e2 = p2 - p0;
            const double crossLen = glm::length(glm::cross(e1, e2));
            if (crossLen < RenderingExperiments::kDegeneratePatchMinCrossLen)
                continue;
        }

        indices.push_back(baseVertexIndex + ia);
        indices.push_back(baseVertexIndex + ib);
        indices.push_back(baseVertexIndex + ic);

        if (pickOut != nullptr)
            pickOut->push_back(PickTriangle{face, flatPositions[ia], flatPositions[ib], flatPositions[ic]});
    }
}

void Patch::AddSolid(const Solid *solid,
                     std::vector<Vertex> &vertices,
                     std::vector<uint32_t> &indices, const AnalysisResults *results,
                     std::vector<PickTriangle> *pickOut) const
{
    for (const Face *face : solid->faces)
        AddFace(face, vertices, indices, true, results, pickOut);
}

std::vector<glm::dvec3> Patch::TessellateCurveToPoints(
    const Curve *curve,
    const glm::dvec3 &start,
    const glm::dvec3 &end,
    int segments) const
{
    std::vector<glm::dvec3> points;
    if (curve == nullptr)
    {
        points.push_back(start);
        points.push_back(end);
        return points;
    }

    for (int i = 0; i <= segments + 1; i++)
    {
        double t = (double)i / segments;
        glm::dvec3 p = curve->Evaluate(t, start, end);
        points.push_back(p);
    }
    return points;
}