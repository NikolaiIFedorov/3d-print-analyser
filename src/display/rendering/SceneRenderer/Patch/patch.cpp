#include "patch.hpp"
#include "RenderingExperiments.hpp"
#include "utils/log.hpp"

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
        AddSolid(&solid, vertices, indices, results, pickOut);

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
            // Edges are already oriented correctly by Face constructor
            const Point *p0 = orientedEdge.GetStart();
            const Point *p1 = orientedEdge.GetEnd();

            // Set projection origin from first point
            if (!originSet)
            {
                projectionOrigin = p0->position;
                originSet = true;
            }

            if (orientedEdge.edge->curve == nullptr)
            {
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

    // Determine face color: use analysis flaw color if available, else default.
    glm::vec3 faceColor = Color::GetFace();
    if (results)
    {
        auto itFlat = results->faceFlaws.find(face);
        if (itFlat != results->faceFlaws.end() && itFlat->second != FaceFlawKind::NONE)
        {
            faceColor = glm::vec3(Color::GetFace(itFlat->second));
        }
        else
        {
            for (const auto &[solid, faceFlawList] : results->faceFlawRanges)
            {
                for (const auto &ff : faceFlawList)
                {
                    if (ff.face == face)
                    {
                        faceColor = glm::vec3(Color::GetFace(ff.flaw));
                        goto colorResolved;
                    }
                }
            }
        colorResolved:;
        }
    }

    uint32_t baseVertexIndex = vertices.size();

    for (const auto &loopPositions : allLoopPositions)
    {
        for (const glm::dvec3 &pos : loopPositions)
        {
            Vertex v;
            v.position = glm::vec3(pos);
            v.color = faceColor;
            v.normal = glm::vec3(faceNormal);

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