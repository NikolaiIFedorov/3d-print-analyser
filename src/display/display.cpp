#include "display.hpp"
#include "imgui_internal.h"
#include "rendering/color.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include "rendering/UIRenderer/UIStyle.hpp"
#include "rendering/UIRenderer/Icons.hpp"
#include "rendering/UIRenderer/ToolPanel.hpp"
#include "rendering/UIRenderer/ToolUserErrorFeedback.hpp"
#include "logic/Analysis/Analysis.hpp"
#include "logic/Analysis/utils/LayerDiffUtils.hpp"
#include "logic/GeometryOps/RingBoolean.hpp"
#include "logic/Import/STLImport.hpp"
#include "logic/Import/STEPImport.hpp"
#include "utils/SystemAccent.hpp"
#include "utils/SystemAppearance.hpp"
#include "utils/ShutdownStackTrace.hpp"

#include <filesystem>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <queue>
#include <cstdio>
#include <cstdlib>
#include <random>
#include "logic/Import/OBJImport.hpp"
#include "logic/Import/ThreeMFImport.hpp"
#include "input/FileImport.hpp"
#include "input/Input.hpp"
#include "rendering/ScenePick.hpp"
#include "Geometry/Curve.hpp"
#include "GeometryOps/RingUtils.hpp"
#include "Geometry/Edge.hpp"
#include "Geometry/Point.hpp"
#include "CalibNominal.hpp"
#include "CalibDistanceType.hpp"
#include "CalibCompensation.hpp"
#include "Structure/StructureTriangulation.hpp"
#include "GeometryValidity.hpp"
#include "Structure/StructureCarve.hpp"
#include <limits>
#include <chrono>
#include <functional>
#include <string_view>
#include "utils/log.hpp"
#include "utils/AppWakeEvent.hpp"

#include "ProjectionDepthMode.hpp"
#include "ViewportDepthExperiments.hpp"
#include "rendering/SceneLighting.hpp"
#include "RenderingExperiments.hpp"
#include "UserTuning.hpp"
#include "scene/scene.hpp"
#include "LengthUnit.hpp"
#include "mapbox/earcut.hpp"

#include <array>
#include <sstream>
#include <string>

#include "imgui.h"

namespace
{

constexpr float kCalibSpanLabelNdcEps = 0.004f;

/// Pan axis-snapping (`snapInput`) stays off until movement exceeds this so the first few pixels are not
/// forced into pure horizontal/vertical lanes (same units as `Pan` deltas after mouse sensitivity scale).
constexpr float kPanSnapTravelFloor = 3.5e-4f;

/// Must match Calibrate parameter row label and `CalibCompensation` literals for error targeting.
constexpr const char kCalibPrintMeasurementLabel[] = "Print measurement";
constexpr const char kCalibPlotMeasurementPointsLabel[] = "Plot measurement points";

/// Font for custom `imguiContent` rows: matches `UIRenderer` (pixel stack when pushed, else pixel/body).
[[nodiscard]] inline ImFont *FontOrInteractiveRow(const UIRenderer &renderer, ImFont *settingsBodyFont)
{
    if (ImFont *f = ImGui::GetFont())
        return f;
    if (ImFont *f = renderer.GetPixelImFont())
        return f;
    return settingsBodyFont;
}

constexpr int kOpenBoundaryBlameCurveSegments = 16;

bool BuildLoopRingPoints(const std::vector<Edge *> &loop, std::vector<glm::dvec3> &ringOut);
glm::dvec3 LoopNewellNormal(const std::vector<glm::dvec3> &ring);
void BuildPlaneBasisFromNormal(const glm::dvec3 &normal, glm::dvec3 &uOut, glm::dvec3 &vOut);

void AppendOpenBoundaryBlameSegment(const glm::dvec3 &a, const glm::dvec3 &b, const glm::vec3 &rgb,
                                    std::vector<Vertex> &verts, std::vector<uint32_t> &indices)
{
    const glm::vec3 lineNormal(0.0f, 0.0f, 1.0f);
    const uint32_t base = static_cast<uint32_t>(verts.size());
    verts.push_back({glm::vec3(a), rgb, lineNormal});
    verts.push_back({glm::vec3(b), rgb, lineNormal});
    indices.push_back(base);
    indices.push_back(base + 1);
}

void AppendOpenBoundaryBlameEdgeGeometry(const Edge *edge, const glm::vec3 &rgb, std::vector<Vertex> &verts,
                                         std::vector<uint32_t> &indices)
{
    if (edge == nullptr || edge->startPoint == nullptr || edge->endPoint == nullptr)
        return;
    const glm::dvec3 p0 = edge->startPoint->position;
    const glm::dvec3 p1 = edge->endPoint->position;
    if (edge->curve != nullptr)
    {
        for (int i = 0; i < kOpenBoundaryBlameCurveSegments; ++i)
        {
            const double t0 = static_cast<double>(i) / kOpenBoundaryBlameCurveSegments;
            const double t1 = static_cast<double>(i + 1) / kOpenBoundaryBlameCurveSegments;
            AppendOpenBoundaryBlameSegment(edge->curve->Evaluate(t0, p0, p1), edge->curve->Evaluate(t1, p0, p1), rgb,
                                         verts, indices);
        }
        return;
    }
    if (!edge->bridgePoints.empty())
    {
        glm::dvec3 prev = p0;
        for (Point *bp : edge->bridgePoints)
        {
            if (bp == nullptr)
                continue;
            AppendOpenBoundaryBlameSegment(prev, bp->position, rgb, verts, indices);
            prev = bp->position;
        }
        AppendOpenBoundaryBlameSegment(prev, p1, rgb, verts, indices);
        return;
    }
    AppendOpenBoundaryBlameSegment(p0, p1, rgb, verts, indices);
}

void AppendOpenBoundaryBlameFaceFillGeometry(const std::vector<std::vector<Edge *>> &faceLoops,
                                             const glm::vec3 &rgb,
                                             std::vector<Vertex> &verts,
                                             std::vector<uint32_t> &indices)
{
    if (faceLoops.empty())
        return;

    std::vector<std::vector<glm::dvec3>> rings3d;
    rings3d.reserve(faceLoops.size());
    for (const std::vector<Edge *> &loop : faceLoops)
    {
        std::vector<glm::dvec3> ring;
        if (!BuildLoopRingPoints(loop, ring) || ring.size() < 3u)
            continue;
        rings3d.push_back(std::move(ring));
    }
    if (rings3d.empty())
        return;

    const glm::dvec3 origin = rings3d.front().front();
    const glm::dvec3 normal = LoopNewellNormal(rings3d.front());
    glm::dvec3 uAxis(0.0);
    glm::dvec3 vAxis(0.0);
    BuildPlaneBasisFromNormal(normal, uAxis, vAxis);

    using EarcutCoord = std::array<double, 2>;
    std::vector<std::vector<EarcutCoord>> polygon;
    polygon.reserve(rings3d.size());
    std::vector<glm::vec3> flatPositions;
    flatPositions.reserve(64);
    for (const std::vector<glm::dvec3> &ring : rings3d)
    {
        if (ring.size() < 3u)
            continue;
        std::vector<EarcutCoord> ring2d;
        ring2d.reserve(ring.size());
        for (const glm::dvec3 &p : ring)
        {
            const glm::dvec3 rel = p - origin;
            ring2d.push_back({glm::dot(rel, uAxis), glm::dot(rel, vAxis)});
            flatPositions.push_back(glm::vec3(p));
        }
        polygon.push_back(std::move(ring2d));
    }
    if (polygon.empty())
        return;

    const std::vector<uint32_t> tri = mapbox::earcut<uint32_t>(polygon);
    if (tri.size() < 3u)
        return;

    const uint32_t base = static_cast<uint32_t>(verts.size());
    const glm::vec3 n = glm::vec3(normal);
    verts.reserve(verts.size() + flatPositions.size());
    for (const glm::vec3 &p : flatPositions)
        verts.push_back({p, rgb, n});
    indices.reserve(indices.size() + tri.size());
    for (uint32_t i : tri)
        indices.push_back(base + i);
}

void CollectRepairCandidateLinearEdgesForSolid(
    const Solid &solid,
    std::vector<Edge *> &outEdges,
    std::size_t &skippedNonLinearEdges)
{
    outEdges.clear();
    skippedNonLinearEdges = 0;

    std::vector<const Edge *> boundaryEdges;
    GeometryValidity::CollectOpenBoundaryEdgesForSolid(solid, boundaryEdges);
    if (boundaryEdges.empty())
        return;

    std::unordered_set<const Edge *> boundarySet;
    std::unordered_set<const Point *> boundaryVertices;
    std::unordered_set<Edge *> seen;
    boundarySet.reserve(boundaryEdges.size());
    boundaryVertices.reserve(boundaryEdges.size() * 2u + 8u);
    outEdges.reserve(boundaryEdges.size() * 2u + 8u);

    auto appendIfLinear = [&](const Edge *ec)
    {
        Edge *e = const_cast<Edge *>(ec);
        if (e == nullptr || e->startPoint == nullptr || e->endPoint == nullptr)
        {
            skippedNonLinearEdges++;
            return;
        }
        if (e->curve != nullptr || !e->bridgePoints.empty())
        {
            skippedNonLinearEdges++;
            return;
        }
        if (!seen.insert(e).second)
            return;
        outEdges.push_back(e);
    };

    for (const Edge *e : boundaryEdges)
    {
        if (e == nullptr)
            continue;
        boundarySet.insert(e);
        if (e->startPoint != nullptr)
            boundaryVertices.insert(e->startPoint);
        if (e->endPoint != nullptr)
            boundaryVertices.insert(e->endPoint);
        appendIfLinear(e);
    }

    for (Face *face : solid.faces)
    {
        if (face == nullptr || face->loops.empty())
            continue;
        for (const std::vector<OrientedEdge> &loop : face->loops)
        {
            for (const OrientedEdge &oe : loop)
            {
                const Edge *e = oe.edge;
                const Point *start = oe.GetStart();
                const Point *end = oe.GetEnd();
                if (e == nullptr || start == nullptr || end == nullptr || boundarySet.contains(e))
                    continue;
                if (!boundaryVertices.contains(start) || !boundaryVertices.contains(end))
                    continue;
                appendIfLinear(e);
            }
        }
    }
}

void CollectClosedLinearBoundaryLoops(
    const std::vector<Edge *> &candidateEdges,
    std::vector<std::vector<Edge *>> &loopsOut,
    std::size_t &skippedLinearBoundaryEdges)
{
    loopsOut.clear();
    skippedLinearBoundaryEdges = 0;
    if (candidateEdges.empty())
        return;

    std::unordered_set<Edge *> allEdges;
    allEdges.reserve(candidateEdges.size());
    std::unordered_map<Point *, std::vector<Edge *>> vertexAdj;
    vertexAdj.reserve(candidateEdges.size() * 2u + 8u);
    std::unordered_map<Point *, int> degree;
    degree.reserve(candidateEdges.size() * 2u + 8u);

    for (Edge *e : candidateEdges)
    {
        if (e == nullptr || e->startPoint == nullptr || e->endPoint == nullptr || e->startPoint == e->endPoint)
        {
            skippedLinearBoundaryEdges++;
            continue;
        }
        if (!allEdges.insert(e).second)
            continue;
        vertexAdj[e->startPoint].push_back(e);
        vertexAdj[e->endPoint].push_back(e);
        degree[e->startPoint]++;
        degree[e->endPoint]++;
    }

    std::unordered_set<Edge *> aliveEdges = allEdges;
    std::queue<Point *> peelQueue;
    for (const auto &kv : degree)
    {
        if (kv.first != nullptr && kv.second < 2)
            peelQueue.push(kv.first);
    }
    while (!peelQueue.empty())
    {
        Point *v = peelQueue.front();
        peelQueue.pop();
        const auto itAdj = vertexAdj.find(v);
        if (itAdj == vertexAdj.end())
            continue;
        for (Edge *e : itAdj->second)
        {
            if (e == nullptr || !aliveEdges.contains(e))
                continue;
            aliveEdges.erase(e);
            for (Point *p : {e->startPoint, e->endPoint})
            {
                if (p == nullptr)
                    continue;
                auto itDeg = degree.find(p);
                if (itDeg == degree.end() || itDeg->second <= 0)
                    continue;
                --itDeg->second;
                if (itDeg->second == 1)
                    peelQueue.push(p);
            }
        }
    }

    std::unordered_set<Edge *> eligibleEdges;
    eligibleEdges.reserve(aliveEdges.size());
    for (Edge *e : aliveEdges)
    {
        if (e == nullptr || e->startPoint == nullptr || e->endPoint == nullptr)
            continue;
        const auto itA = degree.find(e->startPoint);
        const auto itB = degree.find(e->endPoint);
        if (itA != degree.end() && itB != degree.end() && itA->second == 2 && itB->second == 2)
            eligibleEdges.insert(e);
    }

    std::unordered_set<Edge *> usedLoopEdges;
    usedLoopEdges.reserve(eligibleEdges.size());
    for (Edge *seed : eligibleEdges)
    {
        if (seed == nullptr || usedLoopEdges.contains(seed))
            continue;

        std::vector<Edge *> loop;
        loop.reserve(16);
        Point *start = seed->startPoint;
        Point *curr = seed->endPoint;
        Edge *prev = seed;
        loop.push_back(seed);
        usedLoopEdges.insert(seed);

        std::size_t guard = 0;
        bool closed = false;
        while (true)
        {
            if (curr == start)
            {
                closed = true;
                break;
            }
            const auto itAdj = vertexAdj.find(curr);
            if (itAdj == vertexAdj.end())
                break;

            Edge *next = nullptr;
            for (Edge *cand : itAdj->second)
            {
                if (cand == nullptr || cand == prev || !eligibleEdges.contains(cand))
                    continue;
                next = cand;
                break;
            }
            if (next == nullptr || usedLoopEdges.contains(next))
                break;

            Point *nextPoint = nullptr;
            if (next->startPoint == curr)
                nextPoint = next->endPoint;
            else if (next->endPoint == curr)
                nextPoint = next->startPoint;
            if (nextPoint == nullptr)
                break;

            loop.push_back(next);
            usedLoopEdges.insert(next);
            prev = next;
            curr = nextPoint;
            if (++guard > eligibleEdges.size() + 1u)
                break;
        }

        if (closed && loop.size() >= 3u)
            loopsOut.push_back(std::move(loop));
    }

    std::unordered_set<Edge *> loopEdgeSet;
    loopEdgeSet.reserve(candidateEdges.size());
    for (const std::vector<Edge *> &loop : loopsOut)
    {
        for (Edge *e : loop)
        {
            if (e != nullptr)
                loopEdgeSet.insert(e);
        }
    }
    for (Edge *e : allEdges)
    {
        if (e != nullptr && !loopEdgeSet.contains(e))
            skippedLinearBoundaryEdges++;
    }
}

bool BuildLoopRingPoints(const std::vector<Edge *> &loop, std::vector<glm::dvec3> &ringOut)
{
    ringOut.clear();
    if (loop.size() < 3u || loop[0] == nullptr || loop[0]->startPoint == nullptr || loop[0]->endPoint == nullptr)
        return false;

    Point *current = loop[0]->startPoint;
    if (loop.size() > 1u && loop[1] != nullptr)
    {
        Edge *e0 = loop[0];
        Edge *e1 = loop[1];
        if (e0->endPoint == e1->startPoint || e0->endPoint == e1->endPoint)
            current = e0->startPoint;
        else if (e0->startPoint == e1->startPoint || e0->startPoint == e1->endPoint)
            current = e0->endPoint;
    }
    Point *const start = current;
    if (start == nullptr)
        return false;

    ringOut.reserve(loop.size());
    for (Edge *e : loop)
    {
        if (e == nullptr || current == nullptr)
            return false;
        ringOut.push_back(current->position);
        if (e->startPoint == current)
            current = e->endPoint;
        else if (e->endPoint == current)
            current = e->startPoint;
        else
            return false;
    }
    return current == start && ringOut.size() >= 3u;
}

glm::dvec3 LoopNewellNormal(const std::vector<glm::dvec3> &ring)
{
    if (ring.size() < 3u)
        return glm::dvec3(0.0, 0.0, 1.0);
    glm::dvec3 n(0.0);
    for (std::size_t i = 0; i < ring.size(); ++i)
    {
        const glm::dvec3 &a = ring[i];
        const glm::dvec3 &b = ring[(i + 1u) % ring.size()];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    const double len = glm::length(n);
    if (len < 1e-20)
        return glm::dvec3(0.0, 0.0, 1.0);
    return n / len;
}

void BuildPlaneBasisFromNormal(const glm::dvec3 &normal, glm::dvec3 &uOut, glm::dvec3 &vOut)
{
    const glm::dvec3 axisRef = std::abs(normal.z) < 0.9 ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(0.0, 1.0, 0.0);
    uOut = glm::normalize(glm::cross(axisRef, normal));
    vOut = glm::normalize(glm::cross(normal, uOut));
}

double SignedArea2d(const std::vector<glm::dvec2> &poly)
{
    if (poly.size() < 3u)
        return 0.0;
    double a = 0.0;
    for (std::size_t i = 0; i < poly.size(); ++i)
    {
        const glm::dvec2 &p = poly[i];
        const glm::dvec2 &q = poly[(i + 1u) % poly.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return 0.5 * a;
}

bool PointInPolygon2d(const glm::dvec2 &p, const std::vector<glm::dvec2> &poly)
{
    if (poly.size() < 3u)
        return false;

    auto pointOnSegment = [](const glm::dvec2 &q, const glm::dvec2 &a, const glm::dvec2 &b) -> bool
    {
        const glm::dvec2 ab = b - a;
        const glm::dvec2 aq = q - a;
        const double cross = ab.x * aq.y - ab.y * aq.x;
        const double dot = aq.x * ab.x + aq.y * ab.y;
        const double len2 = ab.x * ab.x + ab.y * ab.y;
        const double eps = 1e-9;
        if (std::abs(cross) > eps)
            return false;
        if (dot < -eps || dot > len2 + eps)
            return false;
        return true;
    };

    for (std::size_t i = 0; i < poly.size(); ++i)
    {
        const glm::dvec2 &a = poly[i];
        const glm::dvec2 &b = poly[(i + 1u) % poly.size()];
        if (pointOnSegment(p, a, b))
            return true;
    }

    bool inside = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    {
        const glm::dvec2 &a = poly[i];
        const glm::dvec2 &b = poly[j];
        const bool intersects = ((a.y > p.y) != (b.y > p.y)) &&
                                (p.x < (b.x - a.x) * (p.y - a.y) / ((b.y - a.y) + 1e-20) + a.x);
        if (intersects)
            inside = !inside;
    }
    return inside;
}

bool SolidAlreadyHasEquivalentFaceLoops(const Solid &solid, const std::vector<std::vector<Edge *>> &faceLoops)
{
    if (faceLoops.empty())
        return false;

    std::vector<std::unordered_set<Edge *>> candidateSets;
    candidateSets.reserve(faceLoops.size());
    for (const std::vector<Edge *> &loop : faceLoops)
    {
        std::unordered_set<Edge *> s;
        s.reserve(loop.size());
        for (Edge *e : loop)
        {
            if (e != nullptr)
                s.insert(e);
        }
        if (s.empty())
            return false;
        candidateSets.push_back(std::move(s));
    }

    for (Face *face : solid.faces)
    {
        if (face == nullptr || face->loops.size() != candidateSets.size())
            continue;

        std::vector<bool> used(face->loops.size(), false);
        bool allMatched = true;
        for (const std::unordered_set<Edge *> &cand : candidateSets)
        {
            bool matched = false;
            for (std::size_t i = 0; i < face->loops.size(); ++i)
            {
                if (used[i] || face->loops[i].size() != cand.size())
                    continue;
                bool same = true;
                for (const OrientedEdge &oe : face->loops[i])
                {
                    if (oe.edge == nullptr || !cand.contains(oe.edge))
                    {
                        same = false;
                        break;
                    }
                }
                if (same)
                {
                    used[i] = true;
                    matched = true;
                    break;
                }
            }
            if (!matched)
            {
                allMatched = false;
                break;
            }
        }
        if (allMatched)
            return true;
    }
    return false;
}

std::vector<std::vector<std::vector<Edge *>>> GroupCapLoopsIntoFaces(const std::vector<std::vector<Edge *>> &loopsIn)
{
    struct LoopGeom
    {
        std::vector<Edge *> edges;
        std::vector<glm::dvec2> ring2d;
        glm::dvec2 sample2d{};
        double absArea = 0.0;
        int planeGroup = -1;
        bool assigned = false;
    };

    std::vector<LoopGeom> loops;
    loops.reserve(loopsIn.size());

    std::vector<glm::dvec3> groupNormals;
    std::vector<double> groupD;
    std::vector<glm::dvec3> groupOrigins;
    std::vector<glm::dvec3> groupU;
    std::vector<glm::dvec3> groupV;

    for (const std::vector<Edge *> &loop : loopsIn)
    {
        std::vector<glm::dvec3> ring3d;
        if (!BuildLoopRingPoints(loop, ring3d))
            continue;
        const glm::dvec3 n = LoopNewellNormal(ring3d);
        const glm::dvec3 origin = ring3d.front();
        const double d = glm::dot(n, origin);

        int group = -1;
        for (std::size_t gi = 0; gi < groupNormals.size(); ++gi)
        {
            const double nd = std::abs(glm::dot(n, groupNormals[gi]));
            const double dd = std::abs(d - groupD[gi]);
            if (nd > 0.995 && dd < 1e-4)
            {
                group = static_cast<int>(gi);
                break;
            }
        }
        if (group < 0)
        {
            group = static_cast<int>(groupNormals.size());
            groupNormals.push_back(n);
            groupD.push_back(d);
            groupOrigins.push_back(origin);
            glm::dvec3 u, v;
            BuildPlaneBasisFromNormal(n, u, v);
            groupU.push_back(u);
            groupV.push_back(v);
        }

        LoopGeom lg;
        lg.edges = loop;
        lg.planeGroup = group;
        lg.ring2d.reserve(ring3d.size());
        for (const glm::dvec3 &p : ring3d)
        {
            const glm::dvec3 dv = p - groupOrigins[group];
            lg.ring2d.emplace_back(glm::dot(dv, groupU[group]), glm::dot(dv, groupV[group]));
        }
        glm::dvec2 centroid2d(0.0);
        for (const glm::dvec2 &uv : lg.ring2d)
            centroid2d += uv;
        centroid2d /= static_cast<double>(lg.ring2d.size());
        lg.sample2d = centroid2d;
        lg.absArea = std::abs(SignedArea2d(lg.ring2d));
        loops.push_back(std::move(lg));
    }

    std::vector<std::vector<std::vector<Edge *>>> faceLoopGroups;
    for (std::size_t gi = 0; gi < groupNormals.size(); ++gi)
    {
        std::vector<int> ids;
        for (std::size_t i = 0; i < loops.size(); ++i)
        {
            if (!loops[i].assigned && loops[i].planeGroup == static_cast<int>(gi))
                ids.push_back(static_cast<int>(i));
        }
        std::sort(ids.begin(), ids.end(), [&](int a, int b)
                  { return loops[a].absArea > loops[b].absArea; });

        for (int outerId : ids)
        {
            if (loops[outerId].assigned)
                continue;
            loops[outerId].assigned = true;
            std::vector<std::vector<Edge *>> oneFace;
            oneFace.push_back(loops[outerId].edges);

            for (int innerId : ids)
            {
                if (innerId == outerId || loops[innerId].assigned)
                    continue;
                if (PointInPolygon2d(loops[innerId].sample2d, loops[outerId].ring2d))
                {
                    loops[innerId].assigned = true;
                    oneFace.push_back(loops[innerId].edges);
                }
            }
            faceLoopGroups.push_back(std::move(oneFace));
        }
    }

    return faceLoopGroups;
}

bool BuildFaceOuterLoop2d(const Face *face,
                          const glm::dvec3 &origin,
                          const glm::dvec3 &uAxis,
                          const glm::dvec3 &vAxis,
                          std::vector<glm::dvec2> &ringOut,
                          glm::dvec2 &centroidOut,
                          double &absAreaOut)
{
    ringOut.clear();
    centroidOut = glm::dvec2(0.0);
    absAreaOut = 0.0;
    if (face == nullptr || face->surface == nullptr || !face->surface->IsPlanar() || face->loops.empty() ||
        face->loops[0].size() < 3u)
        return false;

    const std::vector<OrientedEdge> &outer = face->loops[0];
    ringOut.reserve(outer.size());
    for (const OrientedEdge &oe : outer)
    {
        if (oe.edge == nullptr || oe.GetStart() == nullptr)
            return false;
        const glm::dvec3 d = oe.GetStartPosition() - origin;
        ringOut.emplace_back(glm::dot(d, uAxis), glm::dot(d, vAxis));
    }
    if (ringOut.size() < 3u)
        return false;
    for (const glm::dvec2 &p : ringOut)
        centroidOut += p;
    centroidOut /= static_cast<double>(ringOut.size());
    absAreaOut = std::abs(SignedArea2d(ringOut));
    return absAreaOut > 1e-12;
}

bool IsFaceContainedInCoplanarFace(const Face *innerFace, const Face *outerFace)
{
    if (innerFace == nullptr || outerFace == nullptr || innerFace == outerFace || innerFace->surface == nullptr ||
        outerFace->surface == nullptr || !innerFace->surface->IsPlanar() || !outerFace->surface->IsPlanar())
        return false;

    const glm::dvec3 nIn = glm::normalize(innerFace->GetSurface().GetNormal());
    const glm::dvec3 nOut = glm::normalize(outerFace->GetSurface().GetNormal());
    if (std::abs(glm::dot(nIn, nOut)) < 0.999)
        return false;

    if (innerFace->loops.empty() || innerFace->loops[0].empty() || outerFace->loops.empty() || outerFace->loops[0].empty())
        return false;

    const glm::dvec3 origin = innerFace->loops[0][0].GetStartPosition();
    const double dIn = glm::dot(nIn, origin);
    const double dOut = glm::dot(nIn, outerFace->loops[0][0].GetStartPosition());
    if (std::abs(dIn - dOut) > 1e-4)
        return false;

    glm::dvec3 uAxis, vAxis;
    BuildPlaneBasisFromNormal(nIn, uAxis, vAxis);
    std::vector<glm::dvec2> innerRing;
    std::vector<glm::dvec2> outerRing;
    glm::dvec2 innerCentroid(0.0), outerCentroid(0.0);
    double innerArea = 0.0, outerArea = 0.0;
    if (!BuildFaceOuterLoop2d(innerFace, origin, uAxis, vAxis, innerRing, innerCentroid, innerArea) ||
        !BuildFaceOuterLoop2d(outerFace, origin, uAxis, vAxis, outerRing, outerCentroid, outerArea))
        return false;
    if (outerArea <= innerArea * 1.001)
        return false;
    return PointInPolygon2d(innerCentroid, outerRing);
}

void PruneStandaloneInnerFixCaps(Solid &solid, const std::vector<Face *> &createdCaps)
{
    if (createdCaps.size() < 2u)
        return;

    std::unordered_set<Face *> removeSet;
    std::unordered_map<Face *, Face *> innerToOuter;
    for (Face *inner : createdCaps)
    {
        if (inner == nullptr || inner->loops.size() != 1u)
            continue;
        Face *bestOuter = nullptr;
        double bestOuterArea = std::numeric_limits<double>::max();
        for (Face *outer : createdCaps)
        {
            if (outer == nullptr || outer == inner)
                continue;
            if (IsFaceContainedInCoplanarFace(inner, outer))
            {
                std::vector<glm::dvec2> outerRing;
                glm::dvec2 c(0.0);
                double a = 0.0;
                const glm::dvec3 origin = outer->loops[0][0].GetStartPosition();
                const glm::dvec3 n = glm::normalize(outer->GetSurface().GetNormal());
                glm::dvec3 u, v;
                BuildPlaneBasisFromNormal(n, u, v);
                if (BuildFaceOuterLoop2d(outer, origin, u, v, outerRing, c, a) && a < bestOuterArea)
                {
                    bestOuterArea = a;
                    bestOuter = outer;
                }
            }
        }
        if (bestOuter != nullptr)
        {
            removeSet.insert(inner);
            innerToOuter[inner] = bestOuter;
        }
    }
    if (removeSet.empty())
        return;

    for (Face *f : removeSet)
    {
        if (f == nullptr)
            continue;
        Face *outer = nullptr;
        const auto itOuter = innerToOuter.find(f);
        if (itOuter != innerToOuter.end())
            outer = itOuter->second;
        if (outer != nullptr && !f->loops.empty())
        {
            std::vector<OrientedEdge> innerLoopCopy = f->loops[0];
            outer->loops.push_back(innerLoopCopy);
            for (const OrientedEdge &oe : innerLoopCopy)
            {
                if (oe.edge != nullptr)
                    oe.edge->dependencies.insert(outer);
            }
        }
        for (std::vector<OrientedEdge> &loop : f->loops)
        {
            for (OrientedEdge &oe : loop)
            {
                if (oe.edge != nullptr)
                    oe.edge->dependencies.erase(f);
            }
        }
        f->ClearForRemoval();
    }
    solid.faces.erase(std::remove_if(solid.faces.begin(), solid.faces.end(),
                                     [&](Face *f) { return f != nullptr && removeSet.contains(f); }),
                      solid.faces.end());
}

bool TryBuildPlanarHullCapLoop(Scene *scene,
                               const std::vector<const Edge *> &boundaryEdges,
                               std::vector<Edge *> &loopOut,
                               bool allowEdgeCreate = true)
{
    loopOut.clear();
    if (boundaryEdges.size() < 3u)
        return false;

    std::vector<Point *> points;
    points.reserve(boundaryEdges.size() * 2u);
    std::unordered_set<Point *> seenPoints;
    seenPoints.reserve(boundaryEdges.size() * 2u + 8u);
    glm::dvec3 normalAccum(0.0);
    for (const Edge *e : boundaryEdges)
    {
        if (e == nullptr || e->startPoint == nullptr || e->endPoint == nullptr)
            continue;
        if (seenPoints.insert(e->startPoint).second)
            points.push_back(e->startPoint);
        if (seenPoints.insert(e->endPoint).second)
            points.push_back(e->endPoint);
        for (Face *f : e->dependencies)
        {
            if (f != nullptr && f->surface != nullptr && f->surface->IsPlanar())
                normalAccum += f->GetSurface().GetNormal();
        }
    }
    if (points.size() < 3u)
        return false;

    glm::dvec3 normal = normalAccum;
    if (glm::dot(normal, normal) < 1e-18)
    {
        bool found = false;
        for (std::size_t i = 0; i < points.size() && !found; ++i)
        {
            for (std::size_t j = i + 1; j < points.size() && !found; ++j)
            {
                for (std::size_t k = j + 1; k < points.size() && !found; ++k)
                {
                    const glm::dvec3 n =
                        glm::cross(points[j]->position - points[i]->position, points[k]->position - points[i]->position);
                    if (glm::dot(n, n) > 1e-18)
                    {
                        normal = n;
                        found = true;
                    }
                }
            }
        }
        if (!found)
            return false;
    }
    normal = glm::normalize(normal);

    glm::dvec3 centroid(0.0);
    for (Point *p : points)
        centroid += p->position;
    centroid /= static_cast<double>(points.size());

    const double extentMm = [&]()
    {
        double mx = 0.0;
        for (Point *p : points)
            mx = std::max(mx, glm::length(p->position - centroid));
        return mx;
    }();
    const double planeTol = std::max(1e-6, extentMm * 1e-5);
    for (Point *p : points)
    {
        if (std::abs(glm::dot(normal, p->position - centroid)) > planeTol)
            return false;
    }

    glm::dvec3 axisRef = std::abs(normal.z) < 0.9 ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(0.0, 1.0, 0.0);
    glm::dvec3 u = glm::normalize(glm::cross(axisRef, normal));
    glm::dvec3 v = glm::normalize(glm::cross(normal, u));

    struct HullPoint
    {
        Point *p = nullptr;
        glm::dvec2 uv{};
    };
    std::vector<HullPoint> pts2d;
    pts2d.reserve(points.size());
    for (Point *p : points)
    {
        const glm::dvec3 d = p->position - centroid;
        pts2d.push_back(HullPoint{p, glm::dvec2(glm::dot(d, u), glm::dot(d, v))});
    }
    std::sort(pts2d.begin(), pts2d.end(), [](const HullPoint &a, const HullPoint &b)
              { return a.uv.x < b.uv.x || (a.uv.x == b.uv.x && a.uv.y < b.uv.y); });

    auto cross2 = [](const glm::dvec2 &a, const glm::dvec2 &b, const glm::dvec2 &c)
    { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); };

    std::vector<HullPoint> hull;
    hull.reserve(pts2d.size() * 2u);
    for (const HullPoint &hp : pts2d)
    {
        while (hull.size() >= 2 && cross2(hull[hull.size() - 2].uv, hull[hull.size() - 1].uv, hp.uv) <= 0.0)
            hull.pop_back();
        hull.push_back(hp);
    }
    const std::size_t lowerSz = hull.size();
    for (std::size_t i = pts2d.size(); i-- > 0;)
    {
        const HullPoint &hp = pts2d[i];
        while (hull.size() > lowerSz && cross2(hull[hull.size() - 2].uv, hull[hull.size() - 1].uv, hp.uv) <= 0.0)
            hull.pop_back();
        hull.push_back(hp);
    }
    if (hull.size() < 4u)
        return false;
    hull.pop_back();

    std::unordered_set<Point *> hullPts;
    hullPts.reserve(hull.size());
    for (const HullPoint &hp : hull)
    {
        if (hp.p != nullptr)
            hullPts.insert(hp.p);
    }
    std::vector<HullPoint> ring = hull;
    if (hullPts.size() != points.size())
    {
        // Fallback for non-convex/perimeter-with-collinear-vertex cases:
        // use all planar boundary vertices sorted by polar angle around centroid.
        ring = pts2d;
        std::sort(ring.begin(), ring.end(), [](const HullPoint &a, const HullPoint &b)
                  {
                      const double aa = std::atan2(a.uv.y, a.uv.x);
                      const double bb = std::atan2(b.uv.y, b.uv.x);
                      if (aa != bb)
                          return aa < bb;
                      const double ra2 = a.uv.x * a.uv.x + a.uv.y * a.uv.y;
                      const double rb2 = b.uv.x * b.uv.x + b.uv.y * b.uv.y;
                      return ra2 < rb2;
                  });
    }

    auto findEdge = [&](Point *a, Point *b) -> Edge *
    {
        if (a == nullptr || b == nullptr)
            return nullptr;
        for (const Edge *e : boundaryEdges)
        {
            if (e == nullptr)
                continue;
            if ((e->startPoint == a && e->endPoint == b) || (e->startPoint == b && e->endPoint == a))
                return const_cast<Edge *>(e);
        }
        return (allowEdgeCreate && scene != nullptr) ? scene->CreateEdge(a, b) : nullptr;
    };

    loopOut.reserve(ring.size());
    for (std::size_t i = 0; i < ring.size(); ++i)
    {
        Point *a = ring[i].p;
        Point *b = ring[(i + 1) % ring.size()].p;
        if (a == nullptr || b == nullptr || a == b)
            return false;
        Edge *e = findEdge(a, b);
        if (e == nullptr)
            return false;
        loopOut.push_back(e);
    }
    return loopOut.size() >= 3u;
}

static void TruncateUiInlineMessage(std::string &s, std::size_t maxChars = 72)
{
    if (s.size() <= maxChars)
        return;
    const std::size_t keep = maxChars > 3 ? maxChars - 3 : maxChars;
    s.resize(keep);
    if (maxChars > 3)
        s += "...";
}

/// CGAL `what()` often embeds newlines; our logger also uses cursor-up when the **same** message
/// repeats — per-solid identical warns looked like a flood of blank lines in some terminals.
[[nodiscard]] static std::string SanitizeMessageForSingleLineLog(std::string s)
{
    for (char &c : s)
    {
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    }
    return s;
}

/// Map worker/log diagnostics to stable UI codes; keep messages short and log raw strings separately.
[[nodiscard]] static ToolUserErrorPayload MapStructureCarveRawToUserError(std::string raw)
{
    constexpr const char kInset[] = "Inset (mm)";
    constexpr const char kModel[] = "Model";
    constexpr const char kTool[] = "Structure";

    auto ge = [](const char *code, std::string msg, const char *rel)
    { return ToolUserErrorPayload{std::string(code), std::move(msg), std::string(rel)}; };

    const bool partialPrefix = (raw.size() >= 9 && raw.compare(0, 9, "Partial: ") == 0);
    if (partialPrefix)
    {
        raw.erase(0, 9);
        while (!raw.empty() && raw[0] == ' ')
            raw.erase(0, 1);
    }

    std::string s = SanitizeMessageForSingleLineLog(std::move(raw));
    if (s.size() > 450)
    {
        s.resize(447);
        s += "...";
    }

    auto withPartial = [&](ToolUserErrorPayload p) -> ToolUserErrorPayload
    {
        if (partialPrefix)
            p.message = "Some solids carved, then: " + p.message;
        return p;
    };

    if (s.empty())
        return withPartial(ge("STRUCT_CARVE_FAILED", "Structure carve did not complete successfully.", kTool));

    if (s.find("Scene clone failed") != std::string::npos ||
        (s.find("clone") != std::string::npos &&
         (s.find("Scene") != std::string::npos || s.find("scene") != std::string::npos)))
        return withPartial(ge("STRUCT_SCENE_CLONE", "Could not duplicate the scene for carving.", kTool));

    if (s.find("cancelled") != std::string::npos)
        return withPartial(ge("STRUCT_CANCELLED", "Carve was cancelled.", kTool));

    if (s.find("Could not build triangle soup") != std::string::npos)
        return withPartial(ge("STRUCT_MESH_SOUP", "Could not build a triangle mesh from the solid for carving.", kModel));

    if (s.find("Invalid triangle soup") != std::string::npos ||
        s.find("precondition failed for polygon_soup") != std::string::npos)
        return withPartial(ge("STRUCT_MESH_INVALID",
                               "Mesh data was not valid for CGAL carving (check the model for gaps or bad triangles).",
                               kModel));

    if (s.find("Empty CGAL mesh") != std::string::npos)
        return withPartial(ge("STRUCT_MESH_EMPTY", "CGAL produced an empty mesh from the solid.", kModel));

    if (s.find("CGAL boolean difference failed") != std::string::npos || s.find("corefinement") != std::string::npos)
        return withPartial(ge("STRUCT_BOOLEAN_FAILED",
                               "Boolean carving failed. Try a smaller inset, fewer excluded faces, or a simpler mesh.",
                               kInset));

    if (s.find("No carve prisms were applied") != std::string::npos)
        return withPartial(ge("STRUCT_NO_PRISM",
                               "No carve region was applied (preview geometry may be empty or invalid for the current inset).",
                               kInset));

    if (s.find("Failed to rebuild solid") != std::string::npos)
        return withPartial(ge("STRUCT_REBUILD_FAILED", "Carving ran but rebuilding the scene solid failed.", kTool));

    if (s.rfind("CGAL exception:", 0) == 0)
    {
        std::string tail = s.substr(std::string("CGAL exception:").size());
        while (!tail.empty() && tail[0] == ' ')
            tail.erase(0, 1);
        if (tail.size() > 220)
        {
            tail.resize(217);
            tail += "...";
        }
        std::string msg = "CGAL could not complete this carve.";
        if (!tail.empty())
            msg += " (" + tail + ")";
        return withPartial(ge("STRUCT_CGAL_EXCEPTION", msg, kInset));
    }

    if (s.find("CGAL exception (unknown type)") != std::string::npos)
        return withPartial(ge("STRUCT_CGAL_UNKNOWN", "CGAL failed with an unknown error type.", kInset));

    if (s.find("worker exception") != std::string::npos)
        return withPartial(ge("STRUCT_WORKER_EXCEPTION", "The carve worker failed unexpectedly.", kTool));

    std::string fallback = "Structure carve failed.";
    if (!s.empty() && s.size() <= 240)
        fallback += " (" + s + ")";
    else if (!s.empty())
        fallback += " (" + s.substr(0, 200) + "...)";
    return withPartial(ge("STRUCT_CARVE_FAILED", fallback, kTool));
}

static void ClearStructurePanelHeaderTrailing(RootPanel *uiStructure, UIRenderer &uiRenderer)
{
    if (uiStructure && uiStructure->header.has_value())
    {
        uiStructure->header->trailingCaption.clear();
        uiRenderer.MarkDirty();
    }
}

static void SetStructurePanelHeaderTrailing(RootPanel *uiStructure, UIRenderer &uiRenderer, std::string msg)
{
    if (!uiStructure || !uiStructure->header.has_value())
        return;
    TruncateUiInlineMessage(msg);
    uiStructure->header->trailingCaption = std::move(msg);
    uiRenderer.MarkDirty();
}

/// Two text zones styled like settings theme `Select` pills (text-only segments, hover accent fill).
/// Draw-list primitives use **screen** coordinates (same convention as `UIRenderer` segmented `Select`).
static void DrawSceneEditDualPillRow(float winW, float winH, ImFont *lblFont, const char *left, const char *right,
                                     const std::function<void()> &onLeft, const std::function<void()> &onRight,
                                     int accentDepth)
{
    if (!lblFont)
        lblFont = ImGui::GetFont();
    const float pad = ImGui::GetStyle().FramePadding.x;
    const float lblSize = lblFont->FontSize;
    const float baseH = winH;
    const float pillR = std::round(baseH * 0.35f);
    constexpr float zoneInset = 2.0f; // matches `UIRenderer` segmented Select
    ImDrawList *dl = ImGui::GetWindowDrawList();

    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    const ImVec2 rowOrigin = ImGui::GetCursorScreenPos();

    const float lw0 = lblFont->CalcTextSizeA(lblSize, FLT_MAX, 0.0f, left).x;
    const float lw1 = lblFont->CalcTextSizeA(lblSize, FLT_MAX, 0.0f, right).x;
    float z0 = lw0 + 2.0f * pad + 2.0f * zoneInset;
    float z1 = lw1 + 2.0f * pad + 2.0f * zoneInset;
    const float total = z0 + z1;
    const float surplus = winW - total;
    if (surplus > 0.0f)
    {
        z0 += surplus * 0.5f;
        z1 += surplus * 0.5f;
    }

    float cumX = 0.0f;
    auto drawZone = [&](int idx, float zw, const char *txt, const std::function<void()> &fn)
    {
        const float zx0 = cumX;
        const float zx1 = cumX + zw;
        ImGui::SetCursorPos(ImVec2(zx0, 0.0f));
        ImGui::InvisibleButton((std::string("##scSeg") + std::to_string(idx)).c_str(), ImVec2(zw, baseH));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        if (hovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (clicked)
            fn();
        if (hovered)
        {
            glm::vec4 hc = Color::GetAccent(accentDepth, 0.12f, UIStyle::ACCENT_SAT_MULT_HOVER);
            dl->AddRectFilled(ImVec2(rowOrigin.x + zx0 + 2.f, rowOrigin.y + 2.f),
                              ImVec2(rowOrigin.x + zx1 - 2.f, rowOrigin.y + baseH - 2.f),
                              ImGui::GetColorU32(ImVec4(hc.r, hc.g, hc.b, hc.a)), pillR);
        }
        dl->PushClipRect(ImVec2(rowOrigin.x + zx0 + 2.f, rowOrigin.y),
                         ImVec2(rowOrigin.x + zx1 - 2.f, rowOrigin.y + baseH), true);
        const float tw = lblFont->CalcTextSizeA(lblSize, FLT_MAX, 0.0f, txt).x;
        const float mid = rowOrigin.x + (zx0 + zx1) * 0.5f;
        const float ty = rowOrigin.y + (baseH - lblSize) * 0.5f;
        const int depth = hovered ? 2 : 0;
        glm::vec4 tc = Color::GetUIText(depth);
        dl->AddText(lblFont, lblSize, ImVec2(mid - tw * 0.5f, ty), ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a)), txt);
        dl->PopClipRect();
        cumX += zw;
    };

    drawZone(0, z0, left, onLeft);
    drawZone(1, z1, right, onRight);
}

/// Full-row hit target; clipboard is **value only** when `valueStr` is non-empty, otherwise the label (status text).
void CalibDrawCopyableResultRow(ImDrawList *dl, float x0, float y, float w, float rowH, float pad, ImFont *bodyFont,
                                glm::vec4 tcLabel, glm::vec4 tcValue, const char *label, const char *valueStr,
                                const char *imguiIdSuffix)
{
    std::string clip;
    if (valueStr[0] != '\0')
        clip = valueStr;
    else if (label[0] != '\0')
        clip = label;

    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    ImGui::PushID(imguiIdSuffix);
    ImGui::InvisibleButton("copyRow", ImVec2(w, rowH));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    ImGui::PopID();
    if (clicked && !clip.empty())
        ImGui::SetClipboardText(clip.c_str());
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (hovered)
    {
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        glm::vec4 ac = Color::GetAccent(1, 0.12f, 1.0f);
        dl->AddRectFilled(mn, mx, ImGui::GetColorU32(ImVec4(ac.r, ac.g, ac.b, ac.a)), 4.0f);
    }

    const ImU32 lc = ImGui::GetColorU32(ImVec4(tcLabel.r, tcLabel.g, tcLabel.b, tcLabel.a));
    const ImU32 vc = ImGui::GetColorU32(ImVec4(tcValue.r, tcValue.g, tcValue.b, tcValue.a));
    ImFont *drawFont = bodyFont ? bodyFont : ImGui::GetFont();
    const float fs = drawFont ? drawFont->FontSize : ImGui::GetFontSize();
    if (label[0] != '\0')
    {
        const float labelH = drawFont ? drawFont->CalcTextSizeA(fs, FLT_MAX, 0.0f, label).y : ImGui::CalcTextSize(label).y;
        const float ty = y + std::max(0.0f, (rowH - labelH) * 0.5f);
        dl->AddText(drawFont, fs, ImVec2(x0 + pad, ty), lc, label);
    }
    if (valueStr[0] != '\0')
    {
        const ImVec2 vs = drawFont ? drawFont->CalcTextSizeA(fs, FLT_MAX, 0.0f, valueStr)
                                    : ImGui::CalcTextSize(valueStr);
        const float valueH = vs.y;
        const float ty = y + std::max(0.0f, (rowH - valueH) * 0.5f);
        dl->AddText(drawFont, fs, ImVec2(x0 + w - pad - vs.x, ty), vc, valueStr);
    }
}

[[nodiscard]] bool CalibSpanNdcInsideVisibleViewport(glm::vec3 ndc)
{
    return ndc.x >= -1.0f - kCalibSpanLabelNdcEps && ndc.x <= 1.0f + kCalibSpanLabelNdcEps &&
           ndc.y >= -1.0f - kCalibSpanLabelNdcEps && ndc.y <= 1.0f + kCalibSpanLabelNdcEps &&
           ndc.z >= -1.0f - kCalibSpanLabelNdcEps && ndc.z <= 1.0f + kCalibSpanLabelNdcEps;
}

/// Midpoint along `p0`–`p1` centered on the longest contiguous fragment visible inside the NDC cube,
/// so labels stay on-screen when the segment crosses clip planes (orthogonal approximation along arc-length).
[[nodiscard]] std::optional<glm::dvec3> CalibHoverSpanLabelWorldAlongViewportVisible(const glm::mat4 &vp,
                                                                                     glm::dvec3 p0,
                                                                                     glm::dvec3 p1)
{
    constexpr int kSegments = 64;
    std::array<bool, static_cast<size_t>(kSegments) + 1> inside{};
    for (int i = 0; i <= kSegments; ++i)
    {
        const double t = static_cast<double>(i) / static_cast<double>(kSegments);
        const glm::dvec3 pw = p0 + (p1 - p0) * t;
        const glm::vec4 clip = vp * glm::vec4(glm::vec3(pw), 1.0f);
        if (std::abs(clip.w) < 1e-8f)
        {
            inside[static_cast<size_t>(i)] = false;
            continue;
        }
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        inside[static_cast<size_t>(i)] = CalibSpanNdcInsideVisibleViewport(ndc);
    }

    int bestLen = 0;
    int bestStart = 0;
    int runStart = -1;
    auto flushRun = [&](int runEndExclusive)
    {
        if (runStart < 0)
            return;
        const int len = runEndExclusive - runStart;
        if (len > bestLen)
        {
            bestLen = len;
            bestStart = runStart;
        }
        runStart = -1;
    };

    for (int i = 0; i <= kSegments; ++i)
    {
        if (inside[static_cast<size_t>(i)])
        {
            if (runStart < 0)
                runStart = i;
        }
        else
            flushRun(i);
    }
    flushRun(kSegments + 1);

    if (bestLen <= 0)
        return std::nullopt;

    const double midSample =
        static_cast<double>(bestStart) + 0.5 * static_cast<double>(bestLen - 1);
    const double tMid =
        glm::clamp(midSample / static_cast<double>(kSegments), 0.0, 1.0);
    return p0 + (p1 - p0) * tMid;
}

[[nodiscard]] std::string FormatCalibSpanMmLabel(float nominalMm)
{
    const double mm = static_cast<double>(nominalMm);
    const double nearestWhole = std::round(mm);
    char buf[64];
    if (std::abs(mm - nearestWhole) < 5e-4)
        std::snprintf(buf, sizeof(buf), "%.0f mm", nearestWhole);
    else
        std::snprintf(buf, sizeof(buf), "%.3f mm", mm);
    return std::string(buf);
}

const char *AnalysisWorkerPhaseTitle(uint32_t phaseId)
{
    switch (phaseId)
    {
    case AnalysisUiPhase::Slicing:
        return "Slicing model...";
    case AnalysisUiPhase::Analyzing:
        return "Analysing solid mesh...";
    default:
        return "Analysing model...";
    }
}

std::string ImportProgressLabel(const std::string &phase, float progress01)
{
    if (progress01 < 0.0f || progress01 > 1.0f)
        return phase;

    char buf[256];
    snprintf(buf, sizeof(buf), "%s %.0f%%", phase.c_str(), static_cast<double>(progress01 * 100.0f));
    return std::string(buf);
}

std::string ImportPrerequisiteTitle(float progress01)
{
    if (progress01 < 0.0f || progress01 > 1.0f)
        return "Import a file";

    char buf[64];
    snprintf(buf, sizeof(buf), "Import a file: %.0f%%", static_cast<double>(progress01 * 100.0f));
    return std::string(buf);
}

const Face *ResolveCalibFaceForWorkflow(const Face *pickedFace, const Edge *pickedEdge)
{
    if (pickedFace != nullptr)
        return pickedFace;
    if (pickedEdge == nullptr || pickedEdge->dependencies.empty())
        return nullptr;
    const Face *best = nullptr;
    for (Face *fp : pickedEdge->dependencies)
    {
        const Face *f = fp;
        if (best == nullptr || f < best)
            best = f;
    }
    return best;
}

bool CalibSlotHasPick(const Face *f, const Edge *e)
{
    return f != nullptr || e != nullptr;
}

void GatherCalibLayerHoleInnerEdges(const Scene *scene, const glm::dvec3 &buildDir,
                                    std::unordered_set<const Edge *> &layerHoleInnerEdgesOut)
{
    layerHoleInnerEdgesOut.clear();
    if (scene != nullptr)
        CalibrateDistance::RebuildHoleCalibTopology(*scene, buildDir, layerHoleInnerEdgesOut);
}

[[nodiscard]] bool CalibFacePickPassesWallGate(const Face *face, const Edge *edge, const Scene *scene,
                                               double layerHeightMm, const glm::dvec3 &buildDirWorld)
{
    if (face == nullptr)
        return false;
    if (edge != nullptr && scene != nullptr && layerHeightMm > 0.0 &&
        CalibrateDistance::FaceInFirstLayerSlab(face, scene, layerHeightMm, buildDirWorld) &&
        CalibrateDistance::FaceIsLayerCapParallelBuild(face, buildDirWorld))
        return true;
    return CalibrateDistance::FaceNormalPerpendicularToBuild(face, buildDirWorld);
}

/// Squared ray→segment distance threshold (mm²) for snapping Calibrate picks to boundary edges on the
/// first-layer cap only (`PickCalibrateAtPixel`).
constexpr double kCalibEdgePickMaxDistSqMm = 36.0; // 6 mm

bool CalibSecondPickAcceptsHit(const Face *slot1Face, const Edge *slot1Edge,
                               const Face *firstResolved, const Face *hitFace, const Edge *hitEdge,
                               const Scene *scene, double layerHeightMm, const glm::dvec3 &buildDirWorld,
                               const std::unordered_set<const Edge *> &layerHoleInnerEdges)
{
    const Face *cand = ResolveCalibFaceForWorkflow(hitFace, hitEdge);
    if (cand == nullptr)
        return false;
    if (firstResolved == nullptr)
        return true;

    if (slot1Face != nullptr && slot1Edge != nullptr && scene != nullptr && layerHeightMm > 0.0 &&
        CalibrateDistance::FaceInFirstLayerSlab(slot1Face, scene, layerHeightMm, buildDirWorld) &&
        CalibrateDistance::FaceIsLayerCapParallelBuild(slot1Face, buildDirWorld) &&
        CalibrateNominal::EdgeBelongsToFace(slot1Edge, slot1Face))
    {
        if (hitEdge == nullptr)
            return false;
        if (hitFace != slot1Face)
            return false;
        if (!CalibrateNominal::EdgeBelongsToFace(hitEdge, slot1Face))
            return false;
        if (hitEdge == slot1Edge)
            return false;
        return CalibrateNominal::EdgesAreParallelForCalib(slot1Edge, hitEdge);
    }

    if (!CalibrateDistance::FaceNormalPerpendicularToBuild(firstResolved, buildDirWorld) ||
        !CalibrateDistance::FaceNormalPerpendicularToBuild(cand, buildDirWorld))
        return false;

    if (!CalibrateNominal::NormalsAlignedForCalibPick(firstResolved, cand))
        return false;
    if (scene != nullptr)
    {
        const CalibWorkflow w1 =
            CalibrateDistance::ClassifyFace(firstResolved, scene, layerHeightMm, buildDirWorld,
                                            layerHoleInnerEdges);
        const CalibWorkflow w2 =
            CalibrateDistance::ClassifyFace(cand, scene, layerHeightMm, buildDirWorld, layerHoleInnerEdges);
        if (!CalibrateDistance::CalibSecondPickWorkflowsCompatible(w1, w2))
            return false;
    }
    return true;
}

bool AccumulateFaceViewDirection(glm::dvec3 &sum, const Face *face)
{
    if (face == nullptr || !face->HasGeometry())
        return false;

    glm::dvec3 normal = face->GetSurface().GetNormal();
    const double len = glm::length(normal);
    if (!(len > 1e-12) || !std::isfinite(normal.x))
        return false;

    sum += normal / len;
    return true;
}

std::optional<glm::vec3> NormalizeViewDirection(const glm::dvec3 &sum)
{
    const double len = glm::length(sum);
    if (!(len > 1e-8) || !std::isfinite(sum.x))
        return std::nullopt;
    return glm::vec3(sum / len);
}

} // namespace

namespace
{
// Set false to restore legacy ±100000 ortho depth (wider slab, coarser depth steps).
inline constexpr bool kTightenOrthoClipPlanes = true;

/// Ray `o + h * d` (world axis from origin: `d` is column of V⁻¹ omitted — here `d = V_linear * axis`).
/// Returns max `h >= 0` inside the ortho view slab in view space (symmetric XY, Z in [zLo,zHi]).
static float RayOrthoSlabMaxPositiveH(const glm::vec3 &o, const glm::vec3 &d,
                                      float halfW, float halfH,
                                      float zLo, float zHi)
{
    float tEnter = 0.0f;
    float tExit = 1.0e30f;

    auto clip = [&](float po, float pd, float lo, float hi)
    {
        if (std::fabs(pd) < 1e-12f)
        {
            if (po < lo || po > hi)
                tExit = -1.0f;
            return;
        }
        const float inv = 1.0f / pd;
        float t0 = (lo - po) * inv;
        float t1 = (hi - po) * inv;
        if (t0 > t1)
            std::swap(t0, t1);
        tEnter = std::max(tEnter, t0);
        tExit = std::min(tExit, t1);
    };

    clip(o.x, d.x, -halfW, halfW);
    clip(o.y, d.y, -halfH, halfH);
    clip(o.z, d.z, zLo, zHi);

    if (tExit < tEnter || tExit < 0.0f)
        return 0.0f;
    const float enterClamped = std::max(0.0f, tEnter);
    if (tExit < enterClamped)
        return 0.0f;
    return tExit;
}

/// World-space half-length of axis lines — must match `ViewportRenderer` mesh and ortho clip.
/// Extent follows the ortho frustum along each principal direction from the world origin so axes
/// reach the viewport edges after rotation/pan; still at least the grid diameter for huge grids.
inline float OrthoClipAxisWorldHalfExtent(const Camera &cam)
{
    const float gridReach = Color::GRID_EXTENT * 2.0f;

    const glm::mat4 V = cam.GetViewMatrix();
    const float halfW = cam.orthoSize * std::fabs(cam.aspectRatio);
    const float halfH = cam.orthoSize;
    const float zLo = std::min(cam.nearPlane, cam.farPlane);
    const float zHi = std::max(cam.nearPlane, cam.farPlane);

    const glm::vec4 o4 = V * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    const float ow = std::max(1e-12f, std::fabs(o4.w));
    const glm::vec3 o = glm::vec3(o4) / ow;

    float best = 1.0f;
    for (int ax = 0; ax < 3; ++ax)
    {
        for (float s : {-1.0f, 1.0f})
        {
            glm::vec3 wd(0.0f);
            wd[ax] = s;
            const glm::vec3 d = glm::vec3(V * glm::vec4(wd, 0.0f));
            if (glm::length(d) < 1e-12f)
                continue;
            const float hExit = RayOrthoSlabMaxPositiveH(o, d, halfW, halfH, zLo, zHi);
            best = std::max(best, hExit);
        }
    }

    const float withMargin = best * 1.08f + 2.0f;
    return std::min(std::max(gridReach, withMargin), 1.0e6f);
}

void ApplyOrthoClipFromViewBounds(Camera &camera, Scene *scene, float axisWorldHalfExtent)
{
    if (!kTightenOrthoClipPlanes)
    {
        camera.nearPlane = -100000.0f;
        camera.farPlane = 100000.0f;
        return;
    }

    glm::mat4 V = camera.GetViewMatrix();
    float minZ = std::numeric_limits<float>::infinity();
    float maxZ = -std::numeric_limits<float>::infinity();

    auto addWorld = [&](const glm::vec3 &p)
    {
        glm::vec4 v = V * glm::vec4(p, 1.0f);
        const float w = std::max(1e-12f, std::abs(v.w));
        const float z = v.z / w;
        minZ = std::min(minZ, z);
        maxZ = std::max(maxZ, z);
    };

    if (scene != nullptr)
    {
        for (const auto &pt : scene->points)
            addWorld(glm::vec3(pt.position));

        // Patches tessellate curved edges to vertices that lie outside the segment between endpoints.
        // Include those envelopes in view-space Z so tightened near/far does not clip when zoomed in.
        constexpr int kCurveClipSamplesPerEdge = 24;
        const double denom =
            std::max(1.0, static_cast<double>(std::max(1, kCurveClipSamplesPerEdge)));
        for (const Edge &edge : scene->edges)
        {
            if (edge.startPoint == nullptr || edge.endPoint == nullptr)
                continue;

            for (Point *bp : edge.bridgePoints)
            {
                if (bp != nullptr)
                    addWorld(glm::vec3(bp->position));
            }

            if (edge.curve == nullptr)
                continue;

            const glm::dvec3 edgeStart(edge.startPoint->position);
            const glm::dvec3 edgeEnd(edge.endPoint->position);

            for (int i = 0; i <= kCurveClipSamplesPerEdge; ++i)
            {
                const double t = static_cast<double>(i) / denom;
                const glm::dvec3 p(edge.curve->Evaluate(t, edgeStart, edgeEnd));
                addWorld(glm::vec3(p));
            }
        }
    }

    const float ext = Color::GRID_EXTENT;
    addWorld(glm::vec3(-ext, -ext, 0.0f));
    addWorld(glm::vec3(ext, ext, 0.0f));
    addWorld(glm::vec3(-ext, ext, 0.0f));
    addWorld(glm::vec3(ext, -ext, 0.0f));

    const float ax = std::max(1.0f, axisWorldHalfExtent);
    addWorld(glm::vec3(ax, 0.0f, 0.0f));
    addWorld(glm::vec3(-ax, 0.0f, 0.0f));
    addWorld(glm::vec3(0.0f, ax, 0.0f));
    addWorld(glm::vec3(0.0f, -ax, 0.0f));
    addWorld(glm::vec3(0.0f, 0.0f, ax));
    addWorld(glm::vec3(0.0f, 0.0f, -ax));

    if (!std::isfinite(minZ) || !std::isfinite(maxZ) || minZ >= maxZ)
    {
        camera.nearPlane = -100000.0f;
        camera.farPlane = 100000.0f;
        return;
    }

    const float span = maxZ - minZ;
    // Keep clip bounds conservative so close-up navigation does not clip near surfaces when
    // scene extrema are sparse (e.g. coarse topology vs shaded/tessellated geometry).
    const float pad = std::max(120.0f, span * 0.3f);
    camera.nearPlane = minZ - pad;
    camera.farPlane = maxZ + pad;
}
} // namespace

void Display::ResetAnalysisPipelineTotals()
{
    analysisPipelineDenomTotal = 0;
    analysisWorkerStepsDone.store(0, std::memory_order_relaxed);
    analysisTintStepsDone.store(0, std::memory_order_relaxed);
    analysisWorkerPhaseIdAtomic.store(0, std::memory_order_relaxed);
    analysisTintStepMarkedForRequestId = 0;
    analysisGpuRebuildStepsCache = 0;
}

void Display::RefreshAnalysisPipelineDenominatorFromScene()
{
    if (scene == nullptr)
    {
        analysisPipelineDenomTotal = 0;
        return;
    }

    const uint64_t analyzeSteps = Analysis::Instance().CountAnalyzeSteps(scene);
    const uint64_t gpuTail = 4u;
    const uint64_t gpuSteps = static_cast<uint64_t>(scene->solids.size()) + gpuTail;

    // 1 = queue/hand-off consumed when the worker starts, `analyzeSteps`, 1 = delivering results to renderer,
    // `gpuSteps` = incremental rebuild (solids + loose/repack/upload/pick).
    analysisPipelineDenomTotal = 1 + analyzeSteps + 1 + gpuSteps;
}

void Display::PrimeAnalysisWorkerQueueStepDone()
{
    analysisWorkerStepsDone.store(1, std::memory_order_relaxed);
}

void Display::OnAnalysisWorkerSceneStep(uint32_t phaseId, uint64_t intraSceneStepIndex)
{
    analysisWorkerPhaseIdAtomic.store(phaseId, std::memory_order_relaxed);
    // Queue consumed + in-scene counter: global step = 1 + intraSceneStepIndex (starts at 1 after first bump).
    analysisWorkerStepsDone.store(1u + intraSceneStepIndex, std::memory_order_relaxed);
}

void Display::TryMarkAnalysisTintStepOnce(uint64_t requestId)
{
    if (analysisTintStepMarkedForRequestId == requestId)
        return;
    analysisTintStepMarkedForRequestId = requestId;
    analysisTintStepsDone.fetch_add(1, std::memory_order_relaxed);
}

float Display::SyncViewportAxisForDepthClip()
{
    const float h = OrthoClipAxisWorldHalfExtent(camera);
    if (std::isnan(lastSyncedAxisWorldHalfExtent) ||
        std::abs(h - lastSyncedAxisWorldHalfExtent) >
            std::max(0.5f, 0.015f * std::max(1.0f, h)))
    {
        lastSyncedAxisWorldHalfExtent = h;
        viewportRenderer.SetAxisWorldHalfExtent(h);
        viewportRenderer.RegenerateGrid();
    }
    return h;
}

void Display::SyncGridLayoutFromSettings()
{
    settings.gridCellsAlongAxis = std::clamp(settings.gridCellsAlongAxis, 4.0f, 8192.0f);
    const LengthUnit du = LengthUnitFromIndex(settings.defaultLengthUnit);
    Color::GRID_CELL_SIZE = MillimetersPerUnit(du);
    Color::GRID_EXTENT = 0.5f * settings.gridCellsAlongAxis * Color::GRID_CELL_SIZE;
    lastSyncedAxisWorldHalfExtent = std::numeric_limits<float>::quiet_NaN();
    viewportRenderer.RegenerateGrid();
    (void)SyncViewportAxisForDepthClip();
    renderDirty = true;
}

void Display::ApplyTheme()
{
    bool dark;
    switch (themeMode)
    {
    case ThemeMode::Light:
        dark = false;
        break;
    case ThemeMode::Dark:
        dark = true;
        break;
    default: // ThemeMode::System
        dark = (SDL_GetSystemTheme() != SDL_SYSTEM_THEME_LIGHT);
        break;
    }
    Color::SetAppearance(dark);
    dark ? ImGui::StyleColorsDark() : ImGui::StyleColorsLight();
    uiRenderer.MarkDirty();
    viewportRenderer.RegenerateGrid();
    if (scene && (!scene->solids.empty() || !scene->faces.empty()))
        MarkStyleDirty();
    MarkPickDirty();
}

Display::Display(int16_t width, int16_t height, const char *title) : window(InitWindow(width, height, title)), renderer(GetWindow()), viewportRenderer(GetWindow()), uiRenderer(GetWindow(), "/System/Library/Fonts/SFNS.ttf"), camera(width, height)
{
    scene = &baseScene;
    // Apply system appearance and accent color before any UI is constructed.
    // themeMode defaults to System — SDL_GetSystemTheme() is called inside ApplyTheme().
    {
        float hue, sat;
        if (SystemAccent::GetHueSat(hue, sat))
            Color::SetAccent(hue, sat);
        // Bootstrap: set appearance directly so viewportRenderer gets the right colors before ApplyTheme.
        bool dark = (SDL_GetSystemTheme() != SDL_SYSTEM_THEME_LIGHT);
        Color::SetAppearance(dark);
        viewportRenderer.RegenerateGrid();
    }

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    Color::IsDark() ? ImGui::StyleColorsDark() : ImGui::StyleColorsLight();
    ImFontConfig avenirCfg;
    avenirCfg.FontNo = 4; // Avenir Heavy — used for headers (textDepth >= 3) and as ImGui default
    ImFont *heavyFont = io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Avenir.ttc", 19.0f, &avenirCfg);
    ImFontConfig avenirBookCfg;
    avenirBookCfg.FontNo = 0; // Avenir Book — used for body text (textDepth <= 2)
    ImFont *bodyFont = io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Avenir.ttc", 17.0f, &avenirBookCfg);
    ImFont *pixelFont = io.Fonts->AddFontDefault();
    ImGui_ImplSDL3_InitForOpenGL(window, glContext);
    ImGui_ImplOpenGL3_Init("#version 330");
    uiRenderer.SetPixelImFont(pixelFont);
    uiRenderer.SetBodyImFont(bodyFont);
    uiRenderer.SetHeavyImFont(heavyFont);

    AppWakeEvent::RegisterEventType();
    taskRunner = std::make_unique<TaskRunner>();

    InitUI();
    LoadSettings();
    SDL_AddEventWatch(ResizeEventWatcher, this);

    LOG_VOID("Initialized display");
}

SDL_Window *Display::InitWindow(int16_t width, int16_t height, const char *title)
{
    // `RenderingExperiments::kSdlTrackpadIsTouchOnly`: see comment on that constant (restart required).
    SDL_SetHint(SDL_HINT_TRACKPAD_IS_TOUCH_ONLY,
                RenderingExperiments::kSdlTrackpadIsTouchOnly ? "1" : "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        LOG_FALSE("Failed to initialize SDL: " + std::string(SDL_GetError()));
        return nullptr;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    const int msaaSamples = RenderingExperiments::kGlFramebufferMsaaSamples;
    if (msaaSamples > 0)
    {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, msaaSamples);
    }
    else
    {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
    }

    windowWidth = width;
    windowHeight = height;

    SDL_Window *w = SDL_CreateWindow(title, windowWidth, windowHeight, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!w)
    {
        LOG_FALSE("Failed to create SDL window: " + std::string(SDL_GetError()));
        return nullptr;
    }

    glContext = SDL_GL_CreateContext(w);
    if (!glContext)
    {
        LOG_FALSE("Failed to create GL context: " + std::string(SDL_GetError()));
        return nullptr;
    }

    SDL_GL_MakeCurrent(w, glContext);
    SDL_GL_SetSwapInterval(1);

    return w;
}

bool Display::ResizeEventWatcher(void *userdata, SDL_Event *event)
{
    if (event->type == SDL_EVENT_WINDOW_RESIZED)
    {
        Display *self = static_cast<Display *>(userdata);
        int width = event->window.data1;
        int height = event->window.data2;
        if (height > 0)
        {
            self->SetAspectRatio(width, height);
            self->Render();
        }
    }
    return true;
}

void Display::Shutdown()
{
    ShutdownStackTraceLogIfEnabled("mainthread Display::Shutdown entry");
    SessionLogger::Instance().LogShutdownPhase("display: begin");
    SessionLogger::Instance().LogShutdownPhase("display: ResetStructurePreviewIncrementalState");
    ResetStructurePreviewIncrementalState();
    SessionLogger::Instance().LogShutdownPhase("display: CancelPendingStructureCarveJob");
    CancelPendingStructureCarveJob();
    ShutdownStackTraceLogIfEnabled("mainthread after structure cancel");

    // Import/analysis workers can be stuck inside CGAL or analysis; `~TaskRunner` would join() forever.
    // Drop task handles (non-blocking abandon of in-flight futures), drain + detach workers, then
    // leak the runner.
    SessionLogger::Instance().LogShutdownPhase("display: mainThreadPipeline.Clear");
    mainThreadPipeline.Clear();
    if (pendingImportTask.has_value())
    {
        SessionLogger::Instance().LogShutdownPhase("display: cancel pending import task");
        pendingImportTask->RequestCancel();
        pendingImportTask.reset();
    }
    if (pendingAnalysisTask.has_value())
    {
        SessionLogger::Instance().LogShutdownPhase("display: cancel pending analysis task");
        pendingAnalysisTask->RequestCancel();
        pendingAnalysisTask.reset();
    }
    if (taskRunner)
    {
        SessionLogger::Instance().LogShutdownPhase(
            "display: RequestStopClearQueueAndDetachWorkers + release taskRunner");
        taskRunner->RequestStopClearQueueAndDetachWorkers();
        (void)taskRunner.release();
    }

    SessionLogger::Instance().LogShutdownPhase("display: SaveSettings");
    SaveSettings();
    SessionLogger::Instance().LogShutdownPhase("display: SystemAppearance::ClearChangeCallback");
    SystemAppearance::ClearChangeCallback();
    SessionLogger::Instance().LogShutdownPhase("display: ImGui_ImplOpenGL3_Shutdown");
    ImGui_ImplOpenGL3_Shutdown();
    SessionLogger::Instance().LogShutdownPhase("display: ImGui_ImplSDL3_Shutdown");
    ImGui_ImplSDL3_Shutdown();
    SessionLogger::Instance().LogShutdownPhase("display: ImGui::DestroyContext");
    ImGui::DestroyContext();

    SessionLogger::Instance().LogShutdownPhase("display: SDL_RemoveEventWatch");
    SDL_RemoveEventWatch(ResizeEventWatcher, this);
    SessionLogger::Instance().LogShutdownPhase("display: uiRenderer.Shutdown");
    uiRenderer.Shutdown();
    SessionLogger::Instance().LogShutdownPhase("display: viewportRenderer.Shutdown");
    viewportRenderer.Shutdown();
    ShutdownStackTraceLogIfEnabled("mainthread before renderer.Shutdown");
    SessionLogger::Instance().LogShutdownPhase("display: renderer.Shutdown");
    renderer.Shutdown();
    SessionLogger::Instance().LogShutdownPhase("display: SDL_GL_DestroyContext");
    if (glContext)
    {
        SDL_GL_DestroyContext(glContext);
        glContext = nullptr;
    }
    ShutdownStackTraceLogIfEnabled("mainthread after GL context destroyed");
    SessionLogger::Instance().LogShutdownPhase("display: SDL_DestroyWindow");
    if (window)
    {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    SessionLogger::Instance().LogShutdownPhase("display: SDL_Quit");
    SDL_Quit();
    ShutdownStackTraceLogIfEnabled("mainthread Display::Shutdown before return");
    SessionLogger::Instance().LogShutdownPhase("display: end");
}

void Display::LoadSettings()
{
    Settings loaded;
    if (!loaded.Load(Settings::DefaultPath()))
        return; // No file yet — keep all defaults.

    // Analysis
    overhangAngle = loaded.overhangAngle;
    sharpCornerThreshold = loaded.sharpCornerThreshold;
    instabilityMinWidth = loaded.instabilityMinWidth;
    layerDifferenceMaxAreaDelta = loaded.layerDifferenceMaxAreaDelta;
    layerHeight = loaded.layerHeight;

    // Appearance
    settingsAccentHue = loaded.accentHue;
    settingsAccentSat = loaded.accentSat;
    settingsAccentUseSystem = loaded.accentUseSystem;
    if (!settingsAccentUseSystem)
        Color::SetAccent(settingsAccentHue, settingsAccentSat);

    themeMode = static_cast<ThemeMode>(std::clamp(loaded.themeMode, 0, 2));
    UserTuning::contrast = std::clamp(loaded.contrast, 0.0f, 1.0f);
    UserTuning::DeriveFromContrast();
    Color::SetUiDepthStep(UserTuning::uiDepthStep);
    ApplyTheme();
    MarkStyleDirty();
    MarkPickDirty();

    // Viewport
    settings.gridCellsAlongAxis = loaded.gridCellsAlongAxis;
    settings.gridPlaneTiltMinOpacity = std::clamp(loaded.gridPlaneTiltMinOpacity, 0.0f, 1.0f);
    settings.defaultLengthUnit = std::clamp(loaded.defaultLengthUnit, 0, 3);
    lastSyncedAxisWorldHalfExtent = std::numeric_limits<float>::quiet_NaN();
    SyncGridLayoutFromSettings();
    viewportRenderer.SetGridPlaneTiltMinOpacity(settings.gridPlaneTiltMinOpacity);

    // Navigation
    mouseSensitivity = loaded.mouseSensitivity;
    UserTuning::snap = std::clamp(loaded.snap, 0.0f, 1.0f);
    UserTuning::DeriveFromSnap();

    // Re-run analysis with restored parameters.
    RebuildAnalysis();
    MarkGeometryDirtyAll();

    // Settings UI was built before load — sync pill indices to restored state.
    if (uiAppearanceThemeSelect)
        uiAppearanceThemeSelect->activeIndex = static_cast<int>(themeMode);
    if (uiAppearanceAccentSelect)
        uiAppearanceAccentSelect->activeIndex = settingsAccentUseSystem ? 0 : 1;
    if (uiDefaultLengthUnitSelect)
        uiDefaultLengthUnitSelect->activeIndex = settings.defaultLengthUnit;
    uiRenderer.MarkDirty();
}

void Display::SaveSettings()
{
    settings.overhangAngle = overhangAngle;
    settings.sharpCornerThreshold = sharpCornerThreshold;
    settings.instabilityMinWidth = instabilityMinWidth;
    settings.layerDifferenceMaxAreaDelta = layerDifferenceMaxAreaDelta;
    settings.layerHeight = layerHeight;
    settings.accentHue = settingsAccentHue;
    settings.accentSat = settingsAccentSat;
    settings.accentUseSystem = settingsAccentUseSystem;
    settings.themeMode = static_cast<int>(themeMode);
    settings.contrast = UserTuning::contrast;
    settings.mouseSensitivity = mouseSensitivity;
    settings.snap = UserTuning::snap;
    // defaultLengthUnit is owned by Settings directly when the user changes the Viewport pill.
    settings.Save(Settings::DefaultPath());
}

void Display::RebuildAnalysis()
{
    // One critical section inside Analysis (clear + default analyzers) — avoids nested locks on the pipeline mutex.
    Analysis::Instance().RebuildDefaultAnalyzers(overhangAngle, layerHeight, sharpCornerThreshold,
                                                 instabilityMinWidth, layerDifferenceMaxAreaDelta);
}

void Display::UpdateCamera()
{
    cameraDirty = true;
    renderDirty = true;
}

void Display::Render()
{
    const auto renderStart = std::chrono::steady_clock::now();
    auto bg = Color::GetBase();
    glClearColor(bg.r, bg.g, bg.b, 1.0f);
    if (RenderingExperiments::kReverseZDepth)
    {
        glClearDepth(0.0);
        glDepthFunc(GL_GEQUAL);
    }
    else
    {
        glClearDepth(1.0);
        glDepthFunc(GL_LEQUAL);
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    RefreshCalibSpanOverlayForViewportRender();

    const bool structureUiActive =
        activeTool == ActiveTool::Structure && uiStructure != nullptr && uiStructure->visible;
    const bool structurePreviewStrutsVisible =
        structureUiActive && scene != nullptr && !scene->solids.empty();

    const bool analysisUiActive =
        activeTool == ActiveTool::Analysis && uiAnalysis != nullptr && uiAnalysis->visible;
    const bool analysisDebugLinesVisible =
        analysisUiActive && analysisDebugViewEnabled && scene != nullptr && !scene->solids.empty();

    // Face culling applies only to filled triangles (patches + pick highlight), not grid/lines.
    glDisable(GL_CULL_FACE);

    const bool cullOpaqueTriangles = ViewportDepthExperiments::IsBackFaceCull() ||
                                   RenderingExperiments::kCullBackFacesOpaquePatches;
    if (cullOpaqueTriangles)
    {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
    }

    // Mark only the solid surface pixels in the stencil buffer (value = 1).
    // Lines are excluded — their geometry-shader quads extend beyond silhouettes
    // and would bleed into the stencil, incorrectly clipping axes.
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    renderer.RenderPatches();
    renderer.RenderPickHighlight();
    if (RenderingExperiments::kCalibrateSecondPickDrawInvalidFacePool)
        renderer.RenderPickHighlightCalibInvalid();
    renderer.RenderPickHighlightReject();
    if (cullOpaqueTriangles)
        glDisable(GL_CULL_FACE);

    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP); // stop writing before lines
    if (!RenderingExperiments::kDebugSkipSceneWireframe)
        renderer.RenderWireframe();
    // Structure preview lines: foreground pass with pick highlights; x-ray pass after grid.
    if (structurePreviewStrutsVisible)
        renderer.RenderStructurePreviewLines(5.25f);
    // Analysis debug overlay: raw sliced cross-section loops, same draw ordering as Structure preview.
    if (analysisDebugLinesVisible)
    {
        renderer.RenderAnalysisDebugLines(5.25f);
        renderer.RenderAnalysisDebugTriangles();
    }
    if (analysisUiActive)
        renderer.RenderSharpCornerEdges(5.25f);
    // Calibrate: thick accent lines for committed edge picks (and any other pick-highlight lines).
    renderer.RenderPickHighlightLines(6.0f);

    // Grid after solid + wireframe: stencil==0 only so lines do not bleed onto filled surfaces;
    // clip Z bias still keeps axes > grid > scene where stencil allows.
    viewportRenderer.Render();

    viewportRenderer.RenderAxes();

    glDisable(GL_STENCIL_TEST);
    // Occluded selection: translucent face tint behind nearer geometry, then line overlay (stronger).
    renderer.RenderPickHighlightXray();
    renderer.RenderPickHighlightLinesXray(4.0f);
    renderer.RenderCalibHoverSpanLine(5.0f, false);
    renderer.RenderCalibHoverSpanLine(4.0f, true);
    if (!openBoundaryBlameFaceIndices.empty())
    {
        renderer.RenderOpenBoundaryBlameFace(false);
        renderer.RenderOpenBoundaryBlameFace(true);
    }
    if (!importOpenBoundaryBlameEdges.empty())
    {
        renderer.RenderOpenBoundaryBlameLine(7.0f, false);
        renderer.RenderOpenBoundaryBlameLine(5.0f, true);
    }

    // Nominal span label: GL TextRenderer **before** UI mesh so opaque panels occlude it (ImGui always
    // composites above `UIRenderer`'s GL backgrounds).
    if (calibHoverSpanPreviewActive && !calibHoverSpanLabel.empty())
    {
        GLint vpLabel[4];
        glGetIntegerv(GL_VIEWPORT, vpLabel);
        const glm::mat4 vpMatLabel =
            ProjectionDepthMode::EffectiveProjection(camera.GetProjectionMatrix()) * camera.GetViewMatrix();
        const std::optional<glm::dvec3> labelWorld =
            CalibHoverSpanLabelWorldAlongViewportVisible(vpMatLabel, calibHoverSpanP0, calibHoverSpanP1);
        if (labelWorld.has_value())
        {
            const glm::vec4 clip = vpMatLabel * glm::vec4(glm::vec3(*labelWorld), 1.0f);
            if (std::abs(clip.w) > 1e-8f)
            {
                const glm::vec3 ndc = glm::vec3(clip) / clip.w;
                if (ndc.z >= -1.05f && ndc.z <= 1.05f)
                {
                    const float sx =
                        (ndc.x * 0.5f + 0.5f) * static_cast<float>(vpLabel[2]) + static_cast<float>(vpLabel[0]);
                    const float sy = (1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<float>(vpLabel[3]) +
                                     static_cast<float>(vpLabel[1]);
                    glm::vec4 tc = Color::GetUIText(0);
                    constexpr float kCalibSpanLabelLift = 0.06f;
                    tc.r = tc.g = tc.b = std::clamp(tc.r + kCalibSpanLabelLift, 0.0f, 1.0f);
                    uiRenderer.RenderHudGlyphTextCenteredPx(sx, sy, calibHoverSpanLabel, tc,
                                                            glm::vec4(0.0f, 0.0f, 0.0f, 0.48f));
                }
            }
        }
    }

    const double sceneDrawMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - renderStart).count();
    if (sceneDrawMs >= 1.0)
        LOG_SESSION("Render stage", "scene_draw", "ms", sceneDrawMs);

    // Start ImGui frame
    if (pendingFileTabsRebuild)
    {
        pendingFileTabsRebuild = false;
        RebuildFileTabs();
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    const auto uiStart = std::chrono::steady_clock::now();
    uiRenderer.Render();
    const double uiMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uiStart).count();
    if (uiMs >= 1.0)
        LOG_SESSION("Render stage", "ui_render", "ms", uiMs);

    // Finish ImGui frame
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    const auto swapStart = std::chrono::steady_clock::now();
    SDL_GL_SwapWindow(window);
    const double swapMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - swapStart).count();
    if (swapMs >= 1.0)
        LOG_SESSION("Render stage", "swap_window", "ms", swapMs);
}

void Display::ProcessPendingToolSwitchIfAny()
{
    if (!pendingToolSwitch)
        return;
    pendingToolSwitch = false;
    ClearPickHover();
    ClearCalibrateFacePicks();
    ClearStructureFacePick();
    uiRenderer.MarkDirty();
    const bool showAnalysis = (activeTool == ActiveTool::Analysis);
    const bool showCalibrate = (activeTool == ActiveTool::Calibrate);
    const bool showStructure = (activeTool == ActiveTool::Structure);
    uiAnalysis->visible = showAnalysis;
    uiCalibrate->visible = showCalibrate;
    if (uiStructure)
        uiStructure->visible = showStructure;
    // Leaving the Structure tool without an explicit Cancel/Accept reverts as if Cancelled.
    // Finalize already cleared staging in those paths, so this is a no-op for them.
    if (!showStructure)
        CancelPendingStructureCarveJob();
    if (!showStructure && IsStructureStagingActive())
        RestoreStructureOriginalScene();
    RefreshStructurePreviewForRenderer();
    RefreshAnalysisDebugSlicedView();
    const bool skipHeavyGeomForStructureCommit = structureFinalizeCommitSkipGpuFullRebuild;
    if (structureFinalizeCommitSkipGpuFullRebuild)
        structureFinalizeCommitSkipGpuFullRebuild = false;

    if (analysisEnabled != showAnalysis)
    {
        analysisEnabled = showAnalysis;
        if (skipHeavyGeomForStructureCommit)
        {
            // Structure Accept: carved mesh is already on screen — re-queue analysis only.
            // Do not MarkStyleDirty here: that would run RecolorOnly before async analysis
            // returns (structureAcceptGpuGeometryFresh is only honored when hasAnalysisThisFrame).
            structureAcceptGpuGeometryFresh = true;
            pendingAnalysisAfterGeometryRebuild = true;
            MarkPickDirty();
        }
        else
            UpdateScene();
    }
    else if (!skipHeavyGeomForStructureCommit)
        MarkGeometryDirtyAll();
    else
        MarkPickDirty();
    // Entering Structure with a real model: build the carved staging up-front so the user sees the
    // live carve right away. Must run after `MarkGeometryDirtyAll()` — that path calls
    // `CancelPendingStructureCarveJob()`, which would reset `LaunchPending` and leave the panel
    // stuck on "Preparing carve…" if we scheduled staging first.
    if (showStructure)
        BeginStructureStagingSession();
    uiRenderer.MarkDirty();
    SyncToolbarToolVisualState();
    SyncStructurePanelDerivedVisibility();
}

void Display::UpdateScene()
{
    MarkGeometryDirtyAll();
}

void Display::ScheduleNode(InvalidationNode node)
{
    const size_t idx = static_cast<size_t>(node);
    if ((scheduledNodes & NodeBit(node)) == 0)
        invalidationStats.scheduled[idx]++;
    scheduledNodes |= NodeBit(node);
}

void Display::MarkStyleDirty()
{
    if (pendingAnalysisTask.has_value())
    {
        pendingAnalysisTask->RequestCancel();
        pendingAnalysisTask.reset();
        pendingAnalysisScene = nullptr;
    }
    pendingAnalysisTint.reset();
    activeAnalysisTintForRebuild.reset();
    activeAnalysisTintIdentityForRebuild = 0;
    analysisRequestId++;

    styleDirty = true;
    ScheduleNode(InvalidationNode::Style);
    ScheduleNode(InvalidationNode::Pick);
    ScheduleNode(InvalidationNode::UI);
    renderDirty = true;
}

void Display::MarkGeometryDirtyAll()
{
    if (pendingAnalysisTask.has_value())
    {
        pendingAnalysisTask->RequestCancel();
        pendingAnalysisTask.reset();
        pendingAnalysisScene = nullptr;
    }
    pendingAnalysisTint.reset();
    activeAnalysisTintForRebuild.reset();
    activeAnalysisTintIdentityForRebuild = 0;
    lastCommittedAnalysisForRecolor.reset();
    analysisRequestId++;


    structureAcceptGpuGeometryFresh = false;
    CancelPendingStructureCarveJob();

    geometryDirtyAll = true;
    geometryDirtySolids.clear();
    ScheduleNode(InvalidationNode::Geometry);
    ScheduleNode(InvalidationNode::Analysis);
    ScheduleNode(InvalidationNode::Pick);
    ScheduleNode(InvalidationNode::UI);
    renderDirty = true;
}

void Display::MarkGeometryDirtySolid(const Solid *solid)
{
    if (solid == nullptr)
    {
        MarkGeometryDirtyAll();
        return;
    }
    if (pendingAnalysisTask.has_value())
    {
        pendingAnalysisTask->RequestCancel();
        pendingAnalysisTask.reset();
        pendingAnalysisScene = nullptr;
    }
    pendingAnalysisTint.reset();
    activeAnalysisTintForRebuild.reset();
    activeAnalysisTintIdentityForRebuild = 0;
    lastCommittedAnalysisForRecolor.reset();
    analysisRequestId++;


    CancelPendingStructureCarveJob();

    if (!geometryDirtyAll)
    {
        if (scene != nullptr)
        {
            std::unordered_set<const Solid *> expanded;
            scene->InsertSolidWithCompoundMembers(solid, expanded);
            for (const Solid *sx : expanded)
                geometryDirtySolids.insert(sx);
        }
        else
            geometryDirtySolids.insert(solid);
    }
    ScheduleNode(InvalidationNode::Geometry);
    ScheduleNode(InvalidationNode::Analysis);
    ScheduleNode(InvalidationNode::Pick);
    ScheduleNode(InvalidationNode::UI);
    renderDirty = true;
}

void Display::MarkPickDirty()
{
    pickDirty = true;
    ScheduleNode(InvalidationNode::Pick);
    renderDirty = true;
}

void Display::InvalidationSkip(InvalidationNode node)
{
    invalidationStats.skipped[static_cast<size_t>(node)]++;
}

void Display::InvalidationExec(InvalidationNode node)
{
    invalidationStats.executed[static_cast<size_t>(node)]++;
}

void Display::InvalidationGuardrailViolation()
{
    invalidationStats.guardrailViolations++;
}

void Display::ClearScheduledNodes()
{
    scheduledNodes = 0;
}

void Display::RunPickNode()
{
    // Pick mesh refresh must run after geometry/style has fully settled.
    // Never force a sync fallback rebuild from here; geometry/style work owns rebuild cadence.
    if (geometryDirtyAll || !geometryDirtySolids.empty() || styleDirty)
    {
        InvalidationGuardrailViolation();
        InvalidationSkip(InvalidationNode::Pick);
        return;
    }

    const bool wantPickHighlight =
        pickDirty || hoverPickFace != nullptr || hoverPickEdge != nullptr || calibFacePoint1 != nullptr ||
        calibFacePoint2 != nullptr || calibEdgePoint1 != nullptr || calibEdgePoint2 != nullptr ||
        !structureExcludedFaces.empty() || activeTool == ActiveTool::Structure;
    const bool wantOpenBoundaryBlame = !importOpenBoundaryBlameEdges.empty() || !importOpenBoundaryContextEdges.empty() ||
                                       importOpenBoundaryToolPayload.has_value();

    if (wantPickHighlight)
        RebuildPickHighlightMesh();
    if (wantOpenBoundaryBlame)
    {
        RebuildOpenBoundaryBlameFaceGpuMesh();
        RebuildOpenBoundaryBlameLineGpuMesh();
    }

    if (wantPickHighlight || wantOpenBoundaryBlame)
    {
        InvalidationExec(InvalidationNode::Pick);
        return;
    }
    InvalidationSkip(InvalidationNode::Pick);
}

void Display::RunUiNode()
{
    InvalidationExec(InvalidationNode::UI);
}

Display::AsyncAnalysisResult Display::ProduceAsyncAnalysisFromScene(const Scene *sceneForAnalysis, uint64_t requestId,
                                                                    const TaskRunner::CancellationToken &token)
{
    AsyncAnalysisResult out;
    out.scene = sceneForAnalysis;
    out.requestId = requestId;
    if (token.IsCancellationRequested())
    {
        out.cancelled = true;
        return out;
    }
    PrimeAnalysisWorkerQueueStepDone();
    Analysis::AnalyzeSceneReporter reporter = [this](uint32_t phaseId, uint64_t stepIndex)
    { OnAnalysisWorkerSceneStep(phaseId, stepIndex); };
    out.results = Analysis::Instance().AnalyzeScene(sceneForAnalysis, &reporter);
    if (token.IsCancellationRequested())
    {
        out.cancelled = true;
        return out;
    }
    out.ok = true;
    return out;
}

void Display::PollPendingAnalysisTaskIfReady()
{
    if (!pendingAnalysisTask.has_value())
        return;
    std::optional<AsyncAnalysisResult> analysisReady = pendingAnalysisTask->TryTake(WorkerFuturePollRemainingMs());
    if (!analysisReady.has_value())
        return;
    const uint64_t readyRequestId = analysisReady->requestId;
    pendingAnalysisTask.reset();
    pendingAnalysisScene = nullptr;
    if (analysisReady->ok && !analysisReady->cancelled && analysisReady->scene == scene &&
        analysisReady->requestId == analysisRequestId)
    {
        PruneDefunctAnalysisResults(analysisReady->results, scene);
        pendingAnalysisTint = std::move(*analysisReady);
        TryMarkAnalysisTintStepOnce(readyRequestId);
        styleDirty = true;
        ScheduleNode(InvalidationNode::Style);
        ScheduleNode(InvalidationNode::Analysis);
        ScheduleNode(InvalidationNode::Pick);
        ScheduleNode(InvalidationNode::UI);
        renderDirty = true;
    }
}

void Display::Frame()
{
    workerFuturePollDeadline.emplace(std::chrono::steady_clock::now() + TaskRunner::kUiAsyncFutureCompletionBudget);
    ProcessPendingToolSwitchIfAny();
    ProcessDeferredImportIfAny();
    PollStructureStagingTaskIfReady();
    FlushPendingStructureStagingCarveLaunchIfAny();
    ApplyImportProgressSnapshot();
    ApplyStructureProgressSnapshot();
    const bool ranMainThreadApplyTask = mainThreadPipeline.Process(1.5);
    // The pipeline is budgeted, so a single AppWakeEvent::Push() from the worker that originally
    // enqueued this work only guarantees one Frame() tick. If steps are still queued, re-arm the
    // wake event so the blocking SDL_WaitEventTimeout doesn't strand the remaining steps until a
    // real input event happens to arrive (e.g. import results sitting unapplied with no nudge).
    //
    // A pipeline step that just finished (e.g. import's UpdateScene()) can also set
    // geometryDirtyAll/styleDirty/pickDirty as a side effect. The rebuild block below is gated on
    // `!ranMainThreadApplyTask` so it never runs in the same tick as pipeline work, deferring the
    // actual GPU upload to the next Frame(). Without re-arming here too, that next tick never
    // arrives until a real input event happens to nudge the loop — leaving the new geometry
    // flagged dirty but never uploaded.
    if (mainThreadPipeline.HasPending() ||
        (ranMainThreadApplyTask && (geometryDirtyAll || !geometryDirtySolids.empty() || styleDirty || pickDirty)))
        AppWakeEvent::Push();

    const float axisH = SyncViewportAxisForDepthClip();
    ApplyOrthoClipFromViewBounds(camera, scene, axisH);

    // Always sync projection + view to GPU: `ApplyOrthoClipFromViewBounds` updates near/far
    // every frame from the current view matrix; previously we only pushed matrices when
    // `cameraDirty`, so clip planes and `OpenGLRenderer` projection could diverge.
    renderer.SetCamera(camera);
    viewportRenderer.SetCamera(camera);
    if (cameraDirty)
        cameraDirty = false;

    PollPendingAnalysisTaskIfReady();

    if (analysisEnabled && taskRunner && pendingAnalysisAfterGeometryRebuild && !geometryDirtyAll && geometryDirtySolids.empty() &&
        !styleDirty && !pendingAnalysisTask.has_value() && !activeAnalysisTintForRebuild.has_value())
    {
        pendingAnalysisAfterGeometryRebuild = false;
        const uint64_t requestId = ++analysisRequestId;
        const Scene *sceneForAnalysis = scene;
        pendingAnalysisTask = taskRunner->Submit(
            [this, sceneForAnalysis, requestId](const TaskRunner::CancellationToken &token) -> AsyncAnalysisResult
            { return ProduceAsyncAnalysisFromScene(sceneForAnalysis, requestId, token); });
        pendingAnalysisScene = sceneForAnalysis;
    }

    // Keep processing cards synchronized even when no invalidation pass runs
    // (e.g. import busy / queued states while geometry flags are currently clean).
    const bool hasModelNow = !scene->solids.empty() || !scene->faces.empty();
    const bool geometryOrStyleWorkNow = geometryDirtyAll || !geometryDirtySolids.empty() || styleDirty;
    RefreshToolProcessingCards(hasModelNow, geometryOrStyleWorkNow, ranMainThreadApplyTask);

    if (!ranMainThreadApplyTask && (geometryDirtyAll || !geometryDirtySolids.empty() || styleDirty || pickDirty))
    {
        ScheduleNode(InvalidationNode::Geometry);
        if (styleDirty)
            ScheduleNode(InvalidationNode::Style);
        if (geometryDirtyAll || !geometryDirtySolids.empty() || styleDirty)
            ScheduleNode(InvalidationNode::Analysis);
        ScheduleNode(InvalidationNode::Pick);
        ScheduleNode(InvalidationNode::UI);

        bool hasModel = !scene->solids.empty() || !scene->faces.empty();
        const bool activeHasModel = hasModel && !pendingImportTabActive;
        const bool activePendingImport =
            pendingImportTabActive && (importBusy || pendingImportTask.has_value());

        const bool geometryOrStyleWork = geometryDirtyAll || !geometryDirtySolids.empty() || styleDirty;

        // Toggle Analysis panel sections based on model presence
        if (uiImportPara)
            uiImportPara->visible = activePendingImport || !activeHasModel;
        if (uiResult)
            uiResult->visible = activeHasModel && analysisUiScene == scene;
        if (uiVerdict)
            uiVerdict->visible = activeHasModel && analysisUiScene == scene;
        RefreshToolProcessingCards(hasModel, geometryOrStyleWork, ranMainThreadApplyTask);
        uiRenderer.MarkDirty();

        bool geometryRebuildComplete = true;
        bool hasAnalysisThisFrame = false;
        if (analysisEnabled && pendingAnalysisTint.has_value() && pendingAnalysisTint->scene == scene &&
            styleDirty)
        {
            PruneDefunctAnalysisResults(pendingAnalysisTint->results, scene);
            activeAnalysisTintForRebuild.emplace(std::move(pendingAnalysisTint->results));
            activeAnalysisTintIdentityForRebuild = pendingAnalysisTint->requestId;
            pendingAnalysisTint.reset();
            hasAnalysisThisFrame = true;
            if (!geometryDirtyAll && geometryDirtySolids.empty() && !structureAcceptGpuGeometryFresh)
                geometryDirtyAll = true;
        }
        else
        {
            // Do not queue another worker while a prior run's tint is still being applied to geometry
            // (activeAnalysisTintForRebuild); otherwise the next frame can submit AnalyzeScene again,
            // pendingAnalysisTask flips on, and Result/Verdict hide right after they were shown.
            const bool shouldLaunchAsyncAnalysis =
                taskRunner && geometryOrStyleWork && analysisEnabled && !skipAnalysisForNextGeometryRebuild &&
                !pendingAnalysisTask.has_value() && !pendingAnalysisTint.has_value() &&
                !activeAnalysisTintForRebuild.has_value();
            if (shouldLaunchAsyncAnalysis && !renderer.FullRebuildInProgress())
            {
                pendingAnalysisAfterGeometryRebuild = false;
                const uint64_t requestId = analysisRequestId;
                const Scene *sceneForAnalysis = scene;
                pendingAnalysisTask = taskRunner->Submit(
                    [this, sceneForAnalysis, requestId](const TaskRunner::CancellationToken &token) -> AsyncAnalysisResult
                    { return ProduceAsyncAnalysisFromScene(sceneForAnalysis, requestId, token); });
                pendingAnalysisScene = sceneForAnalysis;
            }
            else if (shouldLaunchAsyncAnalysis)
            {
                // Geometry work is still in-flight; launch analysis as soon as the rebuild drains.
                pendingAnalysisAfterGeometryRebuild = true;
            }
        }

        const AnalysisResults *activeTintPtr =
            activeAnalysisTintForRebuild.has_value() ? &*activeAnalysisTintForRebuild : nullptr;
        const uint64_t activeTintId =
            activeAnalysisTintForRebuild.has_value() ? activeAnalysisTintIdentityForRebuild : 0;

        if (geometryDirtyAll || !geometryDirtySolids.empty())
        {
            RefreshStructurePreviewForRenderer();
            RefreshAnalysisDebugSlicedView();
        }

        if (geometryDirtyAll)
        {
            geometryRebuildComplete =
                renderer.RebuildAllIncremental(scene, activeTintPtr, 2.5, activeTintId);
            InvalidationExec(InvalidationNode::Geometry);
            // Force a draw this tick regardless of completion: if incomplete there's more to
            // come next tick; if this is the completing tick, this is the frame that must
            // actually show the now-finished geometry (renderDirty may otherwise already be
            // false from an earlier tick's Render() call, silently skipping the final draw).
            renderDirty = true;
            if (!geometryRebuildComplete)
            {
                // Keep scheduling geometry work until incremental rebuild completes.
                pickDirty = true;
                // Rebuild is time-budgeted across frames; re-arm the wake event so the
                // blocking SDL_WaitEventTimeout doesn't strand the remaining chunks until a
                // real input event arrives.
                AppWakeEvent::Push();
            }
        }
        else if (!geometryDirtySolids.empty())
        {
            renderer.RebuildSolids(scene, geometryDirtySolids, activeTintPtr);
            InvalidationExec(InvalidationNode::Geometry);
        }
        else if (styleDirty)
        {
            if (structureAcceptGpuGeometryFresh)
            {
                // Post-Accept: keep existing GPU mesh until fresh analysis tint arrives.
                // RecolorOnly / RebuildAllIncremental can re-triangulate annulus tops and crash.
                if (hasAnalysisThisFrame)
                    structureAcceptGpuGeometryFresh = false;
                InvalidationExec(InvalidationNode::Style);
            }
            else if (hasAnalysisThisFrame)
            {
                // Applying fresh analysis often changes rendered topology enough to force
                // a full rebuild. Route through the same incremental path to avoid a hitch.
                geometryDirtyAll = true;
                geometryRebuildComplete =
                    renderer.RebuildAllIncremental(scene, activeTintPtr, 2.5, activeTintId);
                InvalidationExec(InvalidationNode::Geometry);
                // Force a draw this tick regardless of completion — see comment on the other
                // RebuildAllIncremental call site above.
                renderDirty = true;
                if (!geometryRebuildComplete)
                {
                    pickDirty = true;
                    // Rebuild is time-budgeted across frames; re-arm the wake event so the
                    // blocking SDL_WaitEventTimeout doesn't strand the remaining chunks until a
                    // real input event arrives.
                    AppWakeEvent::Push();
                }
            }
            else
            {
                const AnalysisResults *recolorPtr = nullptr;
                if (analysisEnabled && lastCommittedAnalysisForRecolor.has_value())
                    recolorPtr = &*lastCommittedAnalysisForRecolor;
                renderer.RecolorOnly(scene, recolorPtr);
                InvalidationExec(InvalidationNode::Style);
            }
        }

        auto stillPickable = [&](const Face *f) -> bool
        {
            if (f == nullptr)
                return true;
            for (const PickTriangle &tri : renderer.GetPickTriangles())
            {
                if (tri.face == f)
                    return true;
            }
            return false;
        };

        auto stillPickableEdge = [&](const Edge *e) -> bool
        {
            if (e == nullptr)
                return true;
            for (const PickSegment &ps : renderer.GetPickSegments())
            {
                if (ps.edge == e)
                    return true;
            }
            return false;
        };

        if (hoverPickFace != nullptr && !stillPickable(hoverPickFace))
        {
            hoverPickFace = nullptr;
            pickDirty = true;
        }
        if (hoverPickEdge != nullptr && !stillPickableEdge(hoverPickEdge))
        {
            hoverPickEdge = nullptr;
            pickDirty = true;
        }

        bool calibPickInvalidated = false;
        if (calibFacePoint1 != nullptr && !stillPickable(calibFacePoint1))
        {
            calibFacePoint1 = nullptr;
            calibStepPoint1 = Icons::StepState::Active;
            calibPickInvalidated = true;
            pickDirty = true;
        }
        if (calibEdgePoint1 != nullptr && !stillPickableEdge(calibEdgePoint1))
        {
            calibEdgePoint1 = nullptr;
            calibStepPoint1 = Icons::StepState::Active;
            calibPickInvalidated = true;
            pickDirty = true;
        }
        if (calibFacePoint2 != nullptr && !stillPickable(calibFacePoint2))
        {
            calibFacePoint2 = nullptr;
            calibStepPoint2 = Icons::StepState::Active;
            calibPickInvalidated = true;
            pickDirty = true;
        }
        if (calibEdgePoint2 != nullptr && !stillPickableEdge(calibEdgePoint2))
        {
            calibEdgePoint2 = nullptr;
            calibStepPoint2 = Icons::StepState::Active;
            calibPickInvalidated = true;
            pickDirty = true;
        }
        if (!structureExcludedFaces.empty())
        {
            size_t before = structureExcludedFaces.size();
            for (auto it = structureExcludedFaces.begin(); it != structureExcludedFaces.end();)
            {
                const Face *f = *it;
                bool keep = stillPickable(f);
                if (!keep && IsStructureStagingActive() && structureOriginalScene != nullptr)
                {
                    for (const Face &rf : structureOriginalScene->faces)
                    {
                        if (&rf == f)
                        {
                            keep = true;
                            break;
                        }
                    }
                }
                if (!keep)
                    it = structureExcludedFaces.erase(it);
                else
                    ++it;
            }
            if (structureExcludedFaces.size() != before)
            {
                pickDirty = true;
                uiRenderer.MarkDirty();
            }
        }
        if (calibPickInvalidated)
        {
            uiRenderer.MarkDirty();
            RefreshCalibWorkflow();
        }
        else if (CalibSlotHasPick(calibFacePoint1, calibEdgePoint1) &&
                 CalibSlotHasPick(calibFacePoint2, calibEdgePoint2))
        {
            RefreshCalibCompensation();
            uiRenderer.MarkDirty();
        }

        RunPickNode();
        TickStructurePreviewBuildIfNeeded();

        if (hasAnalysisThisFrame)
        {
            AnalysisResults &results = *activeAnalysisTintForRebuild;
            InvalidationExec(InvalidationNode::Analysis);

            // Collect faces per flaw kind for BFS grouping — every detector now populates
            // faceFlawRanges uniformly (no separate per-face or per-edge flaw maps anymore).
            std::unordered_set<const Face *> overhangFaces;
            std::unordered_set<const Face *> sharpCornerFaces;
            std::unordered_set<const Face *> instabilityFaces;
            std::unordered_set<const Face *> layerDifferenceFaces;
            for (const auto &[solid, flaws] : results.faceFlawRanges)
            {
                for (const auto &ff : flaws)
                {
                    if (ff.face == nullptr || !ff.face->HasGeometry())
                        continue;
                    switch (ff.flaw)
                    {
                    case FaceFlawKind::OVERHANG:
                        overhangFaces.insert(ff.face);
                        break;
                    case FaceFlawKind::SHARP_CORNER:
                        sharpCornerFaces.insert(ff.face);
                        break;
                    case FaceFlawKind::INSTABILITY:
                        instabilityFaces.insert(ff.face);
                        break;
                    case FaceFlawKind::LAYER_DIFFERENCE:
                        layerDifferenceFaces.insert(ff.face);
                        break;
                    default:
                        break;
                    }
                }
            }

            // Count connected components of adjacent same-kind flawed faces (one flaw "region"
            // may span several adjacent faces).
            auto countComponents = [](const std::unordered_set<const Face *> &faces) -> size_t
            {
                size_t count = 0;
                std::unordered_set<const Face *> visited;
                for (const Face *seed : faces)
                {
                    if (visited.count(seed))
                        continue;
                    count++;
                    std::queue<const Face *> bfs;
                    bfs.push(seed);
                    visited.insert(seed);
                    while (!bfs.empty())
                    {
                        const Face *current = bfs.front();
                        bfs.pop();
                        for (const auto &loop : current->loops)
                        {
                            for (const auto &oe : loop)
                            {
                                if (oe.edge == nullptr)
                                    continue;
                                for (Face *neighbor : oe.edge->dependencies)
                                {
                                    if (neighbor == nullptr || !neighbor->HasGeometry())
                                        continue;
                                    if (faces.count(neighbor) && !visited.count(neighbor))
                                    {
                                        visited.insert(neighbor);
                                        bfs.push(neighbor);
                                    }
                                }
                            }
                        }
                    }
                }
                return count;
            };

            size_t overhangs = countComponents(overhangFaces);
            size_t sharpCornerCount = countComponents(sharpCornerFaces);
            size_t instabilityCount = countComponents(instabilityFaces);
            size_t layerDifferenceCount = countComponents(layerDifferenceFaces);

            // Compute 3D bounding boxes per flaw category for click-to-frame
            auto expandBounds = [](glm::vec3 &bMin, glm::vec3 &bMax, const glm::dvec3 &p)
            {
                glm::vec3 fp(p);
                bMin = glm::min(bMin, fp);
                bMax = glm::max(bMax, fp);
            };
            auto expandFaceBounds = [&](glm::vec3 &bMin, glm::vec3 &bMax, const Face *face)
            {
                if (face == nullptr || !face->HasGeometry())
                    return;
                for (const auto &loop : face->loops)
                {
                    for (const auto &oe : loop)
                    {
                        if (oe.edge == nullptr || oe.GetStart() == nullptr || oe.GetEnd() == nullptr)
                            continue;
                        expandBounds(bMin, bMax, oe.GetStart()->position);
                        expandBounds(bMin, bMax, oe.GetEnd()->position);
                    }
                }
            };

            constexpr float INF = std::numeric_limits<float>::max();
            glm::vec3 overhangMin(INF), overhangMax(-INF);
            glm::vec3 sharpCornerMin(INF), sharpCornerMax(-INF);
            glm::vec3 instabilityMin(INF), instabilityMax(-INF);
            glm::vec3 layerDifferenceMin(INF), layerDifferenceMax(-INF);
            glm::dvec3 overhangViewDir(0.0);
            glm::dvec3 notEnoughSpaceViewDir(0.0);
            glm::dvec3 instabilityViewDir(0.0);
            glm::dvec3 layerDifferenceViewDir(0.0);

            auto accumulateBounds = [&](const std::unordered_set<const Face *> &faces, glm::vec3 &bMin,
                                        glm::vec3 &bMax, glm::dvec3 &viewDir)
            {
                for (const Face *face : faces)
                {
                    expandFaceBounds(bMin, bMax, face);
                    AccumulateFaceViewDirection(viewDir, face);
                }
            };
            accumulateBounds(overhangFaces, overhangMin, overhangMax, overhangViewDir);
            accumulateBounds(sharpCornerFaces, sharpCornerMin, sharpCornerMax, notEnoughSpaceViewDir);
            accumulateBounds(instabilityFaces, instabilityMin, instabilityMax, instabilityViewDir);
            accumulateBounds(layerDifferenceFaces, layerDifferenceMin, layerDifferenceMax, layerDifferenceViewDir);

            // Bright versions of flaw colors for UI text
            glm::vec4 overhangColor = glm::vec4(Color::GetFace(FaceFlawKind::OVERHANG).r + 0.4f,
                                                Color::GetFace(FaceFlawKind::OVERHANG).g + 0.2f,
                                                Color::GetFace(FaceFlawKind::OVERHANG).b + 0.2f, 1.0f);
            glm::vec4 sharpCornerColor = glm::vec4(Color::GetFace(FaceFlawKind::SHARP_CORNER).r + 0.4f,
                                                      Color::GetFace(FaceFlawKind::SHARP_CORNER).g + 0.3f,
                                                      Color::GetFace(FaceFlawKind::SHARP_CORNER).b + 0.15f, 1.0f);
            glm::vec4 instabilityColor = glm::vec4(Color::GetFace(FaceFlawKind::INSTABILITY).r + 0.4f,
                                                   Color::GetFace(FaceFlawKind::INSTABILITY).g + 0.25f,
                                                   Color::GetFace(FaceFlawKind::INSTABILITY).b + 0.15f, 1.0f);
            glm::vec4 layerDifferenceColor = glm::vec4(Color::GetFace(FaceFlawKind::LAYER_DIFFERENCE).r + 0.3f,
                                                       Color::GetFace(FaceFlawKind::LAYER_DIFFERENCE).g + 0.15f,
                                                       Color::GetFace(FaceFlawKind::LAYER_DIFFERENCE).b + 0.4f, 1.0f);

            auto makeFrameCallback = [this](glm::vec3 bMin, glm::vec3 bMax,
                                            std::optional<glm::vec3> cameraBackDirection) -> std::function<void()>
            {
                if (bMin.x > bMax.x)
                    return nullptr; // no valid bounds
                return [this, bMin, bMax, cameraBackDirection]()
                {
                    if (cameraBackDirection)
                        camera.FrameBoundsFromDirection(bMin, bMax, *cameraBackDirection);
                    else
                        camera.FrameBounds(bMin, bMax);
                    cameraDirty = true;
                    renderDirty = true;
                };
            };

            // Write live flaw state — read each frame by the imguiContent lambdas in uiResult
            flawOverhang.count = overhangs;
            flawOverhang.frameCallback =
                makeFrameCallback(overhangMin, overhangMax, NormalizeViewDirection(overhangViewDir));
            flawSharpCorner.count = sharpCornerCount;
            flawSharpCorner.frameCallback = makeFrameCallback(sharpCornerMin, sharpCornerMax,
                                                                  NormalizeViewDirection(notEnoughSpaceViewDir));
            flawInstability.count = instabilityCount;
            flawInstability.frameCallback =
                makeFrameCallback(instabilityMin, instabilityMax, NormalizeViewDirection(instabilityViewDir));
            flawLayerDifference.count = layerDifferenceCount;
            flawLayerDifference.frameCallback = makeFrameCallback(layerDifferenceMin, layerDifferenceMax,
                                                                   NormalizeViewDirection(layerDifferenceViewDir));

            // Log analysis results to session
            {
                auto &sl = SessionLogger::Instance();
                sl.state.overhangs = overhangs;
                sl.state.sharpCorner = sharpCornerCount;
                sl.state.instabilities = instabilityCount;
                sl.state.layerDifferences = layerDifferenceCount;
                sl.LogAnalysisRun();
            }

            // Two-tier verdict
            bool hasVisual = (overhangs > 0) || (instabilityCount > 0);
            bool hasPrecision = (sharpCornerCount > 0) || (layerDifferenceCount > 0);

            glm::vec4 passColor = glm::vec4(0.4f, 0.8f, 0.4f, 1.0f);
            glm::vec4 failColor = glm::vec4(0.9f, 0.4f, 0.4f, 1.0f);

            std::vector<SectionLine> verdictLines;
            if (hasVisual && hasPrecision)
                verdictLines.push_back({"Some areas might not print well or accurately", "", failColor});
            else if (hasVisual)
                verdictLines.push_back({"Some areas might not print well", "", failColor});
            else if (hasPrecision)
                verdictLines.push_back({"Some areas might not print accurately", "", failColor});
            else
            {
                verdictLines.push_back({"No issues detected", "", passColor});

                // Contextual printing tip based on model geometry
                glm::dvec3 modelMin(std::numeric_limits<double>::max());
                glm::dvec3 modelMax(std::numeric_limits<double>::lowest());
                size_t totalLoops = 0;

                for (const auto &face : scene->faces)
                {
                    if (!face.HasGeometry())
                        continue;
                    totalLoops += face.loops.size();
                    for (const auto &loop : face.loops)
                    {
                        for (const auto &oe : loop)
                        {
                            if (oe.edge == nullptr || oe.edge->startPoint == nullptr)
                                continue;
                            const glm::dvec3 &p = oe.edge->startPoint->position;
                            modelMin = glm::min(modelMin, p);
                            modelMax = glm::max(modelMax, p);
                        }
                    }
                }

                double height = modelMax.z - modelMin.z;
                double footprintX = modelMax.x - modelMin.x;
                double footprintY = modelMax.y - modelMin.y;
                double footprintArea = footprintX * footprintY;
                double footprintDiag = std::sqrt(footprintX * footprintX + footprintY * footprintY);

                struct Tip
                {
                    const char *text;
                    float weight;
                };
                std::vector<Tip> tips = {
                    {"Remember to clean your build plate!", 1.0f},
                    {"A brim can help with bed adhesion", 1.0f},
                    {"Keep your filament dry", 1.0f},
                    {"Level your bed before printing", 1.0f},
                    {"Check your nozzle for wear", 0.5f},
                };

                // Tall & narrow → adhesion tips
                if (footprintDiag > 0 && height / footprintDiag > 1.5)
                {
                    tips[0].weight += 3.0f; // clean build plate
                    tips[1].weight += 3.0f; // brim
                }

                // Large footprint → level bed matters more
                if (footprintArea > 2500.0) // > ~50x50 mm
                    tips[3].weight += 3.0f;

                // Only re-roll the tip when transitioning from flawed → pass
                if (!lastVerdictWasPass)
                {
                    float totalWeight = 0;
                    for (const auto &t : tips)
                        totalWeight += t.weight;

                    static std::mt19937 rng(std::random_device{}());
                    std::uniform_real_distribution<float> dist(0.0f, totalWeight);
                    float r = dist(rng);
                    cachedTip = tips[0].text;
                    float cumulative = 0;
                    for (const auto &t : tips)
                    {
                        cumulative += t.weight;
                        if (r < cumulative)
                        {
                            cachedTip = t.text;
                            break;
                        }
                    }
                }

                glm::vec4 tipColor(0.55f, 0.55f, 0.55f, 1.0f);
                verdictLines.push_back({cachedTip, "", tipColor});
            }

            bool verdictIsPass = !hasVisual && !hasPrecision;
            lastVerdictWasPass = verdictIsPass;
            if (uiVerdict)
                uiVerdict->values = std::move(verdictLines);
            analysisUiScene = scene;
        }
        else
        {
            InvalidationSkip(InvalidationNode::Analysis);
            // Only clear live analysis UI when analysis is off. When analysis is on, geometry/style
            // can stay dirty across many frames (incremental rebuild); clearing here would erase
            // counts/verdict the frame after async results were applied.
            if (geometryOrStyleWork && !analysisEnabled)
            {
                lastVerdictWasPass = false;
                flawOverhang = {};
                flawSharpCorner = {};
                flawInstability = {};
                flawLayerDifference = {};
                if (uiVerdict)
                    uiVerdict->values = {};
            }
        }
        if (geometryOrStyleWork && skipAnalysisForNextGeometryRebuild)
        {
            // Import handoff: keep the first rebuild responsive, then request one
            // follow-up style/analysis pass once geometry is visible.
            skipAnalysisForNextGeometryRebuild = false;
            pendingAnalysisAfterGeometryRebuild = true;
        }
        if (geometryRebuildComplete)
        {
            geometryDirtyAll = false;
            geometryDirtySolids.clear();
            styleDirty = false;
            pickDirty = false;
            if (activeAnalysisTintForRebuild.has_value())
            {
                lastCommittedAnalysisForRecolor = std::move(*activeAnalysisTintForRebuild);
                PruneDefunctAnalysisResults(*lastCommittedAnalysisForRecolor, scene);
                activeAnalysisTintForRebuild.reset();
                renderer.SetSharpCornerEdges(&*lastCommittedAnalysisForRecolor);
            }
            else
            {
                lastCommittedAnalysisForRecolor.reset();
                activeAnalysisTintForRebuild.reset();
                renderer.SetSharpCornerEdges(nullptr);
            }
            activeAnalysisTintIdentityForRebuild = 0;
        }
        else
        {
            // Leave dirty flags latched so the next frame continues incremental rebuild.
            geometryDirtyAll = true;
        }
        // Re-sync cards after tint consumption / verdict fill (first Refresh in this block ran before that work).
        RefreshToolProcessingCards(hasModel, geometryOrStyleWork, ranMainThreadApplyTask);
        InvalidationExec(InvalidationNode::UI);
        ClearScheduledNodes();
    }

    // Hover picking runs from input events (mouse motion when not navigating, and on
    // RMB/MMB release) rather than here. Re-running it on every camera-moved frame was
    // re-triggering the brute-force pick-triangle scan on every pan/orbit/zoom frame,
    // including trackpad gestures and scroll-wheel zoom which bypass the motion handler's
    // nav-drag guard. Hover may go briefly stale mid-gesture; it refreshes on the next
    // real mouse-motion event or nav release.

    if (importBusy || pendingImportTask.has_value())
        renderDirty = true;

    if (renderDirty)
    {
        Render();
        renderDirty = false;
    }
}

void Display::SetAspectRatio(const uint16_t width, const uint16_t height)
{
    windowWidth = static_cast<int16_t>(width);
    windowHeight = static_cast<int16_t>(height);

    // Use physical pixels for the GL viewport so Retina/HiDPI framebuffers
    // are covered correctly. Logical dimensions are still used for the camera
    // and UI (aspect ratio is identical; UI uses its own coordinate space).
    int physW, physH;
    SDL_GetWindowSizeInPixels(window, &physW, &physH);
    glViewport(0, 0, physW, physH);

    camera.SetAspectRatio(static_cast<float>(width) / static_cast<float>(std::max<uint16_t>(1, height)),
                          width, height);
    uiRenderer.SetScreenSize(width, height);

    const float axisH = SyncViewportAxisForDepthClip();
    ApplyOrthoClipFromViewBounds(camera, scene, axisH);

    // Push updated matrices to the renderers immediately. ResizeEventWatcher
    // calls Render() before Frame() has a chance to process cameraDirty, so
    // the renderer must have the fresh projection before that Render() runs.
    renderer.SetCamera(camera);
    viewportRenderer.SetCamera(camera);

    renderDirty = true;
}

void Display::Zoom(const float offsetY, const glm::vec3 &posCursotr)
{
    camera.Zoom(offsetY, posCursotr);

    UpdateCamera();
}

glm::vec3 Display::ScreenToWorld(float pixelX, float pixelY) const
{
    int w, h;
    SDL_GetWindowSize(window, &w, &h);

    float ndcX = 2.0f * pixelX / w - 1.0f;
    float ndcY = 1.0f - 2.0f * pixelY / h;

    glm::vec3 right = camera.orientation * glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 up = camera.orientation * glm::vec3(0.0f, 1.0f, 0.0f);

    return camera.target + right * ndcX * camera.orthoSize * camera.aspectRatio + up * ndcY * camera.orthoSize;
}

PickFilter Display::GetActivePickFilter() const
{
    if (!scene || (scene->solids.empty() && scene->faces.empty()))
        return PickFilter::None;

    if (activeTool == ActiveTool::Calibrate)
    {
        if (!calibPara_Point1 || !calibPara_Point1->visible)
            return PickFilter::None;
        const bool awaitingPoint1Pick = calibPara_Point1->selected;
        const bool awaitingPoint2Pick = calibPara_Point2 && calibPara_Point2->selected;
        if (!awaitingPoint1Pick && !awaitingPoint2Pick)
            return PickFilter::None;
        return PickFilter::Faces;
    }

    if (activeTool == ActiveTool::Structure)
    {
        // Structure tool needs import + closed-volume contract before face picking is offered.
        if (!ImportAllowsGeometryDependentTools())
            return PickFilter::None;
        return PickFilter::Faces;
    }

    return PickFilter::None;
}

void Display::ClearPickHover()
{
    hoverPickFace = nullptr;
    hoverPickEdge = nullptr;
    hoverPickRejected = false;
    MarkPickDirty();
}

void Display::ClearCalibrateFacePicks()
{
    calibFacePoint1 = nullptr;
    calibFacePoint2 = nullptr;
    calibEdgePoint1 = nullptr;
    calibEdgePoint2 = nullptr;
    calibStepPoint1 = Icons::StepState::Active;
    calibStepPoint2 = Icons::StepState::Active;
    if (calibPara_Point1)
        calibPara_Point1->selected = calibPara_Point1->visible;
    if (calibPara_Point2)
        calibPara_Point2->selected = false;
    RefreshCalibWorkflow();
    RefreshCalibDerivedRowVisible();
    uiRenderer.MarkDirty();
    MarkPickDirty();
}

void Display::SetHoverPick(const Face *face, const Edge *edge, bool rejected)
{
    if (hoverPickFace == face && hoverPickEdge == edge && hoverPickRejected == rejected)
    {
        if (face != nullptr || edge != nullptr)
            return;
        if (calibFacePoint1 != nullptr || calibFacePoint2 != nullptr || calibEdgePoint1 != nullptr ||
            calibEdgePoint2 != nullptr || !structureExcludedFaces.empty())
            return;
        if (pickHighlightIndices.empty() && pickHighlightRejectIndices.empty() &&
            pickHighlightCalibInvalidIndices.empty())
            return;
    }
    hoverPickFace = face;
    hoverPickEdge = edge;
    hoverPickRejected = rejected;
    MarkPickDirty();
}

void Display::RebuildPickHighlightMesh()
{
    pickHighlightVertices.clear();
    pickHighlightIndices.clear();
    pickHighlightLineVertices.clear();
    pickHighlightLineIndices.clear();
    pickHighlightRejectVertices.clear();
    pickHighlightRejectIndices.clear();
    pickHighlightCalibInvalidVertices.clear();
    pickHighlightCalibInvalidIndices.clear();

    const std::vector<PickTriangle> &tris = renderer.GetPickTriangles();
    uint32_t nextVert = 0;

    auto appendFaceTrisSolid = [&](const Face *face, const glm::vec3 &rgb)
    {
        if (face == nullptr)
            return;
        for (const PickTriangle &tri : tris)
        {
            if (tri.face != face)
                continue;
            const glm::dvec3 e1 = tri.v1 - tri.v0;
            const glm::dvec3 e2 = tri.v2 - tri.v0;
            glm::vec3 n = glm::normalize(glm::vec3(glm::cross(e1, e2)));
            if (!std::isfinite(static_cast<double>(n.x)) || glm::length(n) < 1e-6f)
                n = glm::vec3(0.0f, 0.0f, 1.0f);

            pickHighlightVertices.push_back({glm::vec3(tri.v0), rgb, n});
            pickHighlightVertices.push_back({glm::vec3(tri.v1), rgb, n});
            pickHighlightVertices.push_back({glm::vec3(tri.v2), rgb, n});
            pickHighlightIndices.push_back(nextVert);
            pickHighlightIndices.push_back(nextVert + 1);
            pickHighlightIndices.push_back(nextVert + 2);
            nextVert += 3;
        }
    };

    auto appendFaceTris = [&](const Face *face, float accentDepthSteps, float satMult)
    {
        if (face == nullptr)
            return;
        const glm::vec3 accent = glm::vec3(Color::GetAccentSteps(accentDepthSteps, 1.0f, satMult));
        for (const PickTriangle &tri : tris)
        {
            if (tri.face != face)
                continue;
            const glm::dvec3 e1 = tri.v1 - tri.v0;
            const glm::dvec3 e2 = tri.v2 - tri.v0;
            glm::vec3 n = glm::normalize(glm::vec3(glm::cross(e1, e2)));
            if (!std::isfinite(static_cast<double>(n.x)) || glm::length(n) < 1e-6f)
                n = glm::vec3(0.0f, 0.0f, 1.0f);

            pickHighlightVertices.push_back({glm::vec3(tri.v0), accent, n});
            pickHighlightVertices.push_back({glm::vec3(tri.v1), accent, n});
            pickHighlightVertices.push_back({glm::vec3(tri.v2), accent, n});
            pickHighlightIndices.push_back(nextVert);
            pickHighlightIndices.push_back(nextVert + 1);
            pickHighlightIndices.push_back(nextVert + 2);
            nextVert += 3;
        }
    };

    // Structure eligibility tint: one normal per face so internal mesh diagonals don't read as lines.
    // `srcTris` defaults to the currently rendered mesh, but the Structure tool overlays the
    // *original* (uncarved) face footprint on top of the carved preview while staging — see below.
    auto appendFaceTrisSurfaceNormal = [&](const Face *face, float accentDepthSteps, float satMult,
                                            const std::vector<PickTriangle> &srcTris)
    {
        if (face == nullptr || face->surface == nullptr)
            return;
        const glm::vec3 accent = glm::vec3(Color::GetAccentSteps(accentDepthSteps, 1.0f, satMult));
        glm::vec3 n = glm::vec3(face->surface->GetNormal());
        const float nLen = glm::length(n);
        if (nLen > 1e-6f)
            n /= nLen;
        else
            n = glm::vec3(0.0f, 0.0f, 1.0f);
        for (const PickTriangle &tri : srcTris)
        {
            if (tri.face != face)
                continue;
            pickHighlightVertices.push_back({glm::vec3(tri.v0), accent, n});
            pickHighlightVertices.push_back({glm::vec3(tri.v1), accent, n});
            pickHighlightVertices.push_back({glm::vec3(tri.v2), accent, n});
            pickHighlightIndices.push_back(nextVert);
            pickHighlightIndices.push_back(nextVert + 1);
            pickHighlightIndices.push_back(nextVert + 2);
            nextVert += 3;
        }
    };

    // Face-only committed picks get the face tint. Edge-snapped picks store the owning face for
    // geometry logic but drawing both reads as "face selected" (hover already suppresses face fill
    // when hoverPickEdge is set — mirror that for committed calibEdgePoint1/2).
    if (calibFacePoint1 != nullptr && calibEdgePoint1 == nullptr)
        appendFaceTris(calibFacePoint1, 1.0f, 0.72f);
    if (calibFacePoint2 != nullptr && calibEdgePoint2 == nullptr)
        appendFaceTris(calibFacePoint2, 1.0f, 0.72f);

    // Structure tool (opt-out model): the tool carves every eligible face by default — that's its
    // decision, not the user's, so it stays unmarked. Only faces the user has actively **excluded**
    // (kept uncarved) are "selected" and get a faint accent tint. While staging shows the carved
    // mesh, exclusions are keyed on original-scene faces — look each carved face up in the
    // precomputed `structureCarvedToOriginal` map (built once per carve, not re-matched per
    // pick/per frame). Faces with no entry are new carve-only geometry (strut walls, cut
    // boundaries) and are never user-excludable, so they never tint.
    //
    // The hovered face is skipped here — `appendFaceTris(hoverDraw, …)` below paints it brighter.
    if (activeTool == ActiveTool::Structure)
    {
        const bool staging = IsStructureStagingActive();

        // Rebuild the eligibility cache lazily here: this method runs whenever pick geometry or
        // hover state changes, which dominates the cases where the eligible set could have shifted.
        // While staging, a carved fragment's *own* size/span is not the right eligibility test — a
        // strut top or leftover sliver can be far smaller than `kMinFaceSpanMm` even though it
        // belongs to a perfectly eligible original panel — so membership in
        // `structureCarvedToOriginal` (built from the *original* face's eligibility) is what counts.
        const std::unordered_set<const Face *> previousEligible = structureEligibleFacesCache;
        structureEligibleFacesCache.clear();
        for (const PickTriangle &tri : tris)
        {
            if (tri.face == nullptr || structureEligibleFacesCache.count(tri.face) > 0)
                continue;
            const bool isCandidate = staging ? structureCarvedToOriginal.count(tri.face) > 0
                                              : IsStructureFaceEligible(tri.face);
            if (isCandidate)
                structureEligibleFacesCache.insert(tri.face);
        }
        for (const Face *f : structureEligibleFacesCache)
        {
            const Face *canonical = f;
            if (staging)
            {
                auto it = structureCarvedToOriginal.find(f);
                if (it == structureCarvedToOriginal.end())
                    continue;
                canonical = it->second;
            }
            if (structureExcludedFaces.count(canonical) == 0)
                continue;
            if (f == hoverPickFace)
                continue;
            appendFaceTrisSurfaceNormal(f, 0.35f, 0.45f, tris);
        }
        // Only re-emit the triangulation preview lines when the eligible set actually shifted —
        // hover-only updates don't change which faces feed `BuildCutOutlinePreviewLines`, so
        // skipping in that case avoids per-frame GPU re-uploads of the preview mesh.
        if (previousEligible != structureEligibleFacesCache)
            RefreshStructurePreviewForRenderer();
    }

    const std::vector<PickSegment> &segPick = renderer.GetPickSegments();
    const glm::vec3 lineNormal(0.0f, 0.0f, 1.0f);
    auto appendEdgeLinesRgb = [&](const Edge *edge, const glm::vec3 &rgb)
    {
        if (edge == nullptr)
            return;
        for (const PickSegment &ps : segPick)
        {
            if (ps.edge != edge)
                continue;
            const uint32_t base = static_cast<uint32_t>(pickHighlightLineVertices.size());
            pickHighlightLineVertices.push_back({glm::vec3(ps.v0), rgb, lineNormal});
            pickHighlightLineVertices.push_back({glm::vec3(ps.v1), rgb, lineNormal});
            pickHighlightLineIndices.push_back(base);
            pickHighlightLineIndices.push_back(base + 1);
        }
    };
    auto appendEdgeLines = [&](const Edge *edge, float accentDepthSteps, float satMult)
    {
        if (edge == nullptr)
            return;
        appendEdgeLinesRgb(edge, glm::vec3(Color::GetAccentSteps(accentDepthSteps, 1.0f, satMult)));
    };

    appendEdgeLines(calibEdgePoint1, 1.0f, 0.72f);
    appendEdgeLines(calibEdgePoint2, 1.0f, 0.72f);

    const bool calibSecondPickConstrained =
        activeTool == ActiveTool::Calibrate && calibPara_Point2 && calibPara_Point2->selected &&
        CalibSlotHasPick(calibFacePoint1, calibEdgePoint1);
    const Face *firstForInvalidPool =
        calibSecondPickConstrained ? ResolveCalibFaceForWorkflow(calibFacePoint1, calibEdgePoint1) : nullptr;
    if (firstForInvalidPool != nullptr && RenderingExperiments::kCalibrateSecondPickDrawInvalidFacePool)
    {
        std::unordered_set<const Edge *> layerHoleInnerEdges;
        const glm::dvec3 calibBuildDir = CalibrateDistance::DefaultCalibrateBuildDirection();
        GatherCalibLayerHoleInnerEdges(scene, calibBuildDir, layerHoleInnerEdges);
        const double layerMm = static_cast<double>(layerHeight);

        const bool elephantEdgeSecondPickMode =
            calibEdgePoint1 != nullptr && scene != nullptr &&
            CalibrateDistance::FaceInFirstLayerSlab(firstForInvalidPool, scene, layerMm, calibBuildDir) &&
            CalibrateDistance::FaceIsLayerCapParallelBuild(firstForInvalidPool, calibBuildDir);

        if (!elephantEdgeSecondPickMode)
        {
            std::unordered_set<const Face *> invalidFaces;
            invalidFaces.reserve(std::min(static_cast<size_t>(256), tris.size() / 2 + 1));
            const CalibWorkflow wFirst =
                scene != nullptr ? CalibrateDistance::ClassifyFace(firstForInvalidPool, scene, layerMm, calibBuildDir,
                                                                   layerHoleInnerEdges)
                                 : CalibWorkflow::Contour;
            for (const PickTriangle &tri : tris)
            {
                const Face *f = tri.face;
                if (f == nullptr)
                    continue;
                if (f == calibFacePoint1 || f == calibFacePoint2)
                    continue;
                if (!CalibrateDistance::FaceNormalPerpendicularToBuild(f, calibBuildDir))
                {
                    invalidFaces.insert(f);
                    continue;
                }
                if (!CalibrateNominal::NormalsAlignedForCalibPick(firstForInvalidPool, f))
                {
                    invalidFaces.insert(f);
                    continue;
                }
                if (scene != nullptr)
                {
                    const CalibWorkflow wf =
                        CalibrateDistance::ClassifyFace(f, scene, layerMm, calibBuildDir, layerHoleInnerEdges);
                    if (!CalibrateDistance::CalibSecondPickWorkflowsCompatible(wFirst, wf))
                        invalidFaces.insert(f);
                }
            }
            if (!invalidFaces.empty())
            {
                // Pool tint must sit near **lit** patch luminance: SRC_ALPHA blend is
                // `α*src + (1-α)*dst`. If src is much lighter than dst, smaller α darkens; larger α
                // lightens — unrelated to “see-through”. Anchor to face albedo + typical diffuse.
                const glm::vec3 albedo = Color::GetFace();
                constexpr float kTypicalDiffuse = 0.55f;
                const glm::vec3 approxLit =
                    glm::min(albedo * (1.0f + SceneLighting::SceneMeshBrightenAmount() * kTypicalDiffuse),
                             glm::vec3(1.0f));
                const glm::vec3 coolShift = glm::vec3(approxLit.r * 0.92f, approxLit.g * 0.96f,
                                                      std::min(1.0f, approxLit.b * 1.1f + 0.02f));
                const glm::vec3 poolTint = glm::mix(approxLit, coolShift, 0.38f);
                uint32_t iv = 0;
                for (const PickTriangle &tri : tris)
                {
                    if (invalidFaces.find(tri.face) == invalidFaces.end())
                        continue;
                    const glm::dvec3 e1 = tri.v1 - tri.v0;
                    const glm::dvec3 e2 = tri.v2 - tri.v0;
                    glm::vec3 n = glm::normalize(glm::vec3(glm::cross(e1, e2)));
                    if (!std::isfinite(static_cast<double>(n.x)) || glm::length(n) < 1e-6f)
                        n = glm::vec3(0.0f, 0.0f, 1.0f);

                    pickHighlightCalibInvalidVertices.push_back({glm::vec3(tri.v0), poolTint, n});
                    pickHighlightCalibInvalidVertices.push_back({glm::vec3(tri.v1), poolTint, n});
                    pickHighlightCalibInvalidVertices.push_back({glm::vec3(tri.v2), poolTint, n});
                    pickHighlightCalibInvalidIndices.push_back(iv);
                    pickHighlightCalibInvalidIndices.push_back(iv + 1);
                    pickHighlightCalibInvalidIndices.push_back(iv + 2);
                    iv += 3;
                }
            }
        }
    }

    {
        const Face *hoverDraw = hoverPickFace;
        if (hoverDraw == calibFacePoint1 || hoverDraw == calibFacePoint2)
            hoverDraw = nullptr;
        // Calibrate edge snap: show edge hover alone — face fill hides whether the hit is edge vs face.
        const bool calibrateEdgeHover =
            activeTool == ActiveTool::Calibrate && hoverPickEdge != nullptr;
        if (hoverDraw != nullptr && !calibrateEdgeHover)
        {
            if (hoverPickRejected)
            {
                const int grayDepth =
                    Color::IsDark() ? RenderingExperiments::kCalibrateRejectHoverGrayUiDepthDark
                                      : RenderingExperiments::kCalibrateRejectHoverGrayUiDepthLight;
                appendFaceTrisSolid(hoverDraw, glm::vec3(Color::GetUI(grayDepth, 1.0f)));
            }
            else
                appendFaceTris(hoverDraw, 0.5f, 0.5f);
        }
    }
    const uint32_t xrayFaceHighlightIndexCount = static_cast<uint32_t>(pickHighlightIndices.size());

    const Edge *hoverEdgeDraw = hoverPickEdge;
    if (hoverEdgeDraw == calibEdgePoint1 || hoverEdgeDraw == calibEdgePoint2)
        hoverEdgeDraw = nullptr;
    if (hoverEdgeDraw != nullptr && activeTool == ActiveTool::Calibrate && hoverPickRejected)
    {
        const int grayDepth =
            Color::IsDark() ? RenderingExperiments::kCalibrateRejectHoverGrayUiDepthDark
                              : RenderingExperiments::kCalibrateRejectHoverGrayUiDepthLight;
        appendEdgeLinesRgb(hoverEdgeDraw, glm::vec3(Color::GetUI(grayDepth, 1.0f)));
    }
    else
        appendEdgeLines(hoverEdgeDraw, 0.5f, 0.5f);
    const uint32_t xrayEdgeHighlightIndexCount = static_cast<uint32_t>(pickHighlightLineIndices.size());

    renderer.UploadPickHighlightMesh(pickHighlightVertices, pickHighlightIndices, xrayFaceHighlightIndexCount);
    renderer.UploadPickHighlightLineMesh(pickHighlightLineVertices, pickHighlightLineIndices,
                                         xrayEdgeHighlightIndexCount);
    renderer.UploadPickHighlightRejectMesh(pickHighlightRejectVertices, pickHighlightRejectIndices);
    renderer.UploadPickHighlightCalibInvalidMesh(pickHighlightCalibInvalidVertices, pickHighlightCalibInvalidIndices);
}

void Display::RefreshCalibSpanOverlayForViewportRender()
{
    calibHoverSpanPreviewActive = false;
    calibHoverSpanLabel.clear();
    calibHoverSpanP0 = glm::dvec3(0.0);
    calibHoverSpanP1 = glm::dvec3(0.0);

    std::vector<Vertex> calibHoverSpanVerts;
    std::vector<uint32_t> calibHoverSpanIdx;

    const auto upload = [&]() { renderer.UploadCalibHoverSpanLineMesh(calibHoverSpanVerts, calibHoverSpanIdx); };

    if (activeTool != ActiveTool::Calibrate || !calibPara_Point1 || !calibPara_Point1->visible || scene == nullptr ||
        (scene->solids.empty() && scene->faces.empty()))
    {
        upload();
        return;
    }

    const glm::vec3 rgb = glm::vec3(Color::GetAccentSteps(0.75f, 1.0f, 0.55f));
    const glm::vec3 lineNormal(0.0f, 0.0f, 1.0f);

    const Face *f1 = ResolveCalibFaceForWorkflow(calibFacePoint1, calibEdgePoint1);
    const Face *f2 = ResolveCalibFaceForWorkflow(calibFacePoint2, calibEdgePoint2);

    const bool hasBoth = CalibSlotHasPick(calibFacePoint1, calibEdgePoint1) &&
                         CalibSlotHasPick(calibFacePoint2, calibEdgePoint2);

    if (hasBoth && f1 != nullptr && f2 != nullptr)
    {
        const bool elephantEdges = calibFacePoint1 == calibFacePoint2 && calibEdgePoint1 != nullptr &&
                                   calibEdgePoint2 != nullptr &&
                                   CalibrateNominal::EdgesAreParallelForCalib(calibEdgePoint1, calibEdgePoint2);
        const CalibrateNominal::SpanPreview sp =
            elephantEdges ? CalibrateNominal::SpanPreviewBetweenParallelEdgesOnFace(f1, calibEdgePoint1,
                                                                                   calibEdgePoint2)
                          : CalibrateNominal::SpanPreviewBetweenFaces(f1, f2);
        if (!sp.valid)
        {
            upload();
            return;
        }
        calibHoverSpanPreviewActive = true;
        calibHoverSpanLabel = FormatCalibSpanMmLabel(sp.nominalMm);
        calibHoverSpanP0 = sp.p0;
        calibHoverSpanP1 = sp.p1;
        calibHoverSpanVerts.push_back({glm::vec3(sp.p0), rgb, lineNormal});
        calibHoverSpanVerts.push_back({glm::vec3(sp.p1), rgb, lineNormal});
        calibHoverSpanIdx.push_back(0);
        calibHoverSpanIdx.push_back(1);
        upload();
        return;
    }

    const bool awaitingSecond =
        calibPara_Point2 && calibPara_Point2->selected && CalibSlotHasPick(calibFacePoint1, calibEdgePoint1);

    if (!awaitingSecond || f1 == nullptr)
    {
        upload();
        return;
    }

    const glm::dvec3 calibBuildDirAwait = CalibrateDistance::DefaultCalibrateBuildDirection();
    const double layerMmAwait = static_cast<double>(layerHeight);

    if (hoverPickFace != nullptr && calibEdgePoint1 != nullptr && hoverPickEdge != nullptr &&
        calibFacePoint1 != nullptr && hoverPickFace == calibFacePoint1 &&
        CalibrateDistance::FaceInFirstLayerSlab(calibFacePoint1, scene, layerMmAwait, calibBuildDirAwait) &&
        CalibrateDistance::FaceIsLayerCapParallelBuild(calibFacePoint1, calibBuildDirAwait))
    {
        const CalibrateNominal::SpanPreview sp =
            CalibrateNominal::SpanPreviewBetweenParallelEdgesOnFace(f1, calibEdgePoint1, hoverPickEdge);
        if (sp.valid)
        {
            calibHoverSpanPreviewActive = true;
            calibHoverSpanLabel = FormatCalibSpanMmLabel(sp.nominalMm);
            calibHoverSpanP0 = sp.p0;
            calibHoverSpanP1 = sp.p1;
            calibHoverSpanVerts.push_back({glm::vec3(sp.p0), rgb, lineNormal});
            calibHoverSpanVerts.push_back({glm::vec3(sp.p1), rgb, lineNormal});
            calibHoverSpanIdx.push_back(0);
            calibHoverSpanIdx.push_back(1);
            upload();
            return;
        }
    }

    if (hoverPickFace != nullptr)
    {
        const CalibrateNominal::SpanPreview sp = CalibrateNominal::SpanPreviewBetweenFaces(f1, hoverPickFace);
        if (sp.valid)
        {
            calibHoverSpanPreviewActive = true;
            calibHoverSpanLabel = FormatCalibSpanMmLabel(sp.nominalMm);
            calibHoverSpanP0 = sp.p0;
            calibHoverSpanP1 = sp.p1;
            calibHoverSpanVerts.push_back({glm::vec3(sp.p0), rgb, lineNormal});
            calibHoverSpanVerts.push_back({glm::vec3(sp.p1), rgb, lineNormal});
            calibHoverSpanIdx.push_back(0);
            calibHoverSpanIdx.push_back(1);
            upload();
            return;
        }
    }

    float mx = 0.0f;
    float my = 0.0f;
    SDL_GetMouseState(&mx, &my);
    int w = 0;
    int h = 0;
    SDL_GetWindowSize(window, &w, &h);
    glm::dvec3 ro;
    glm::dvec3 rd;
    ScenePick::OrthoPickRay(camera, w, h, mx, my, ro, rd);
    const glm::dvec3 centroid = CalibrateNominal::FaceCentroidWorld(f1);
    const double rdLen = glm::length(rd);
    if (rdLen < 1e-30)
    {
        upload();
        return;
    }
    const glm::dvec3 rdUnit = rd / rdLen;
    const double t = -glm::dot(ro - centroid, rdUnit) / rdLen;
    const glm::dvec3 hit = ro + rd * t;

    calibHoverSpanP0 = centroid;
    calibHoverSpanP1 = hit;
    calibHoverSpanVerts.push_back({glm::vec3(centroid), rgb, lineNormal});
    calibHoverSpanVerts.push_back({glm::vec3(hit), rgb, lineNormal});
    calibHoverSpanIdx.push_back(0);
    calibHoverSpanIdx.push_back(1);
    upload();
}

Display::CalibPickHit Display::PickCalibrateAtPixel(float pixelX, float pixelY) const
{
    CalibPickHit out;
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    glm::dvec3 ro, rd;
    ScenePick::OrthoPickRay(camera, w, h, pixelX, pixelY, ro, rd);

    double faceT = 0.0;
    out.face = ScenePick::PickClosestFace(renderer.GetPickTriangles(), ro, rd, PickFilter::Faces, &faceT);
    if (out.face == nullptr || scene == nullptr)
        return out;

    const glm::dvec3 calibBuildDir = CalibrateDistance::DefaultCalibrateBuildDirection();
    const double layerMm = static_cast<double>(layerHeight);
    const bool capForEdgeSnap =
        CalibrateDistance::FaceInFirstLayerSlab(out.face, scene, layerMm, calibBuildDir) &&
        CalibrateDistance::FaceIsLayerCapParallelBuild(out.face, calibBuildDir);

    if (capForEdgeSnap)
    {
        thread_local std::vector<PickSegment> faceSegScratch;
        faceSegScratch.clear();
        for (const PickSegment &ps : renderer.GetPickSegments())
        {
            if (ps.edge != nullptr && CalibrateNominal::EdgeBelongsToFace(ps.edge, out.face))
                faceSegScratch.push_back(ps);
        }
        if (!faceSegScratch.empty())
        {
            double rayT = 0.0;
            double distSq = 0.0;
            const Edge *edgeHit = ScenePick::PickClosestEdgeAlongRay(faceSegScratch, ro, rd, kCalibEdgePickMaxDistSqMm,
                                                                     &rayT, &distSq);
            if (edgeHit != nullptr)
                out.edge = edgeHit;
        }
    }
    return out;
}

void Display::UpdatePickHover(float pixelX, float pixelY)
{
    ImGuiIO &io = ImGui::GetIO();
    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(nullptr, nullptr);
    const bool viewportNav = (buttons & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) != 0 ||
                             (buttons & SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE)) != 0;

    if (io.WantCaptureMouse || HitTestUI(pixelX, pixelY) || HitTestImGui(pixelX, pixelY) || viewportNav)
    {
        SetHoverPick(nullptr, nullptr);
        return;
    }
    if (GetActivePickFilter() == PickFilter::None)
    {
        SetHoverPick(nullptr, nullptr);
        return;
    }

    if (activeTool == ActiveTool::Structure)
    {
        const StructurePickHit structHit = PickStructureAtPixel(pixelX, pixelY);
        SetHoverPick(structHit.face, nullptr, structHit.face != nullptr && !structHit.eligible);
        return;
    }

    const CalibPickHit hit = PickCalibrateAtPixel(pixelX, pixelY);
    const glm::dvec3 calibBuildDir = CalibrateDistance::DefaultCalibrateBuildDirection();
    const double layerMm = static_cast<double>(layerHeight);

    if (calibPara_Point1 && calibPara_Point1->selected &&
        !CalibSlotHasPick(calibFacePoint1, calibEdgePoint1))
    {
        if (hit.face != nullptr &&
            !CalibFacePickPassesWallGate(hit.face, hit.edge, scene, layerMm, calibBuildDir))
        {
            SetHoverPick(hit.face, hit.edge, true);
            return;
        }
    }

    if (calibPara_Point2 && calibPara_Point2->selected && CalibSlotHasPick(calibFacePoint1, calibEdgePoint1))
    {
        const Face *firstResolved = ResolveCalibFaceForWorkflow(calibFacePoint1, calibEdgePoint1);
        std::unordered_set<const Edge *> layerHoleInnerEdges;
        GatherCalibLayerHoleInnerEdges(scene, calibBuildDir, layerHoleInnerEdges);
        if (hit.face != nullptr &&
            !CalibSecondPickAcceptsHit(calibFacePoint1, calibEdgePoint1, firstResolved, hit.face, hit.edge,
                                       scene, layerMm, calibBuildDir, layerHoleInnerEdges))
        {
            SetHoverPick(hit.face, hit.edge, true);
            return;
        }
    }
    SetHoverPick(hit.face, hit.edge, false);
}

void Display::TryCommitCalibrateFacePick(float pixelX, float pixelY)
{
    if (activeTool != ActiveTool::Calibrate)
        return;
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantCaptureMouse || HitTestUI(pixelX, pixelY) || HitTestImGui(pixelX, pixelY))
        return;
    if (!calibPara_Point1 || !calibPara_Point1->visible)
        return;

    const CalibPickHit hit = PickCalibrateAtPixel(pixelX, pixelY);
    if (hit.face == nullptr)
        return;

    const glm::dvec3 calibBuildDirCommit = CalibrateDistance::DefaultCalibrateBuildDirection();
    const double layerMmCommit = static_cast<double>(layerHeight);
    if (!CalibFacePickPassesWallGate(hit.face, hit.edge, scene, layerMmCommit, calibBuildDirCommit))
        return;

    if (calibPara_Point1->selected)
    {
        calibFacePoint1 = hit.face;
        calibEdgePoint1 = hit.edge;
        calibStepPoint1 = Icons::StepState::Done;
        calibPara_Point1->selected = false;
        if (calibPara_Point2)
            calibPara_Point2->selected = !CalibSlotHasPick(calibFacePoint2, calibEdgePoint2);
    }
    else if (calibPara_Point2 && calibPara_Point2->selected)
    {
        const Face *firstResolved = ResolveCalibFaceForWorkflow(calibFacePoint1, calibEdgePoint1);
        const glm::dvec3 calibBuildDir = CalibrateDistance::DefaultCalibrateBuildDirection();
        std::unordered_set<const Edge *> layerHoleInnerEdges;
        GatherCalibLayerHoleInnerEdges(scene, calibBuildDir, layerHoleInnerEdges);
        const double layerMm = static_cast<double>(layerHeight);
        if (!CalibSecondPickAcceptsHit(calibFacePoint1, calibEdgePoint1, firstResolved, hit.face, hit.edge, scene,
                                       layerMm, calibBuildDir, layerHoleInnerEdges))
            return;
        calibFacePoint2 = hit.face;
        calibEdgePoint2 = hit.edge;
        calibStepPoint2 = Icons::StepState::Done;
        calibPara_Point2->selected = false;
        calibPara_Point1->selected = !CalibSlotHasPick(calibFacePoint1, calibEdgePoint1);
    }
    else
        return;

    RefreshCalibWorkflow();
    uiRenderer.MarkDirty();
    MarkPickDirty();
}

namespace
{

// Minimum upward-component for a face to be eligible for triangulation. Anything below this is
// considered side-facing / downward — the XY footprint projection collapses. Slanted upward faces
// are included: inset runs on the face plane, then the footprint is projected to XY and carved with
// a vertical prism. See documentation/implementations/structure_face_triangulation_2026-05-11.md.
constexpr double kStructureMinUpComponent = StructureTriangulation::kMinUpComponent;

// Coarse lower bound on the face's world-space span. Faces smaller than this cannot host even a
// single inset-with-strip, so picking them up just to immediately reject is pointless.
constexpr double kStructureMinFaceSpanMm = StructureTriangulation::kMinFaceSpanMm;

double StructureFaceMaxSpanMm(const Face *face)
{
    if (face == nullptr || face->loops.empty())
        return 0.0;
    glm::dvec3 mn(std::numeric_limits<double>::max());
    glm::dvec3 mx(-std::numeric_limits<double>::max());
    for (const auto &loop : face->loops)
    {
        for (const OrientedEdge &oe : loop)
        {
            const glm::dvec3 p = oe.GetStartPosition();
            mn = glm::min(mn, p);
            mx = glm::max(mx, p);
        }
    }
    const glm::dvec3 ext = mx - mn;
    return std::max({ext.x, ext.y, ext.z});
}

double StructureFaceMaxProjectedSpanXyMm(const Face *face)
{
    if (face == nullptr || face->loops.empty())
        return 0.0;
    double minX = std::numeric_limits<double>::max();
    double maxX = -std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxY = -std::numeric_limits<double>::max();
    for (const auto &loop : face->loops)
    {
        for (const OrientedEdge &oe : loop)
        {
            const glm::dvec3 p = oe.GetStartPosition();
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
        }
    }
    return std::max(maxX - minX, maxY - minY);
}

glm::dvec3 StructureFaceCentroidWorld(const Face *face)
{
    if (face == nullptr || face->loops.empty())
        return glm::dvec3(0.0);
    glm::dvec3 sum(0.0);
    int count = 0;
    for (const OrientedEdge &oe : face->loops[0])
    {
        sum += oe.GetStartPosition();
        ++count;
    }
    return count > 0 ? sum / static_cast<double>(count) : glm::dvec3(0.0);
}

} // namespace

bool Display::IsStructureFaceEligible(const Face *face, std::string *outReason) const
{
    auto setReason = [outReason](const char *r)
    {
        if (outReason != nullptr)
            *outReason = r;
    };
    if (face == nullptr)
    {
        setReason("");
        return false;
    }
    if (face->surface == nullptr || !face->surface->IsPlanar())
    {
        setReason("Face is not planar — only flat faces can be triangulated.");
        return false;
    }
    if (face->loops.empty())
    {
        setReason("Face has no boundary loops.");
        return false;
    }
    const glm::dvec3 n = face->surface->GetNormal();
    if (n.z < kStructureMinUpComponent)
    {
        setReason("Face is too vertical — the XY footprint would collapse.");
        return false;
    }
    if (StructureFaceMaxSpanMm(face) < kStructureMinFaceSpanMm)
    {
        setReason("Face is too small to triangulate.");
        return false;
    }
    if (StructureFaceMaxProjectedSpanXyMm(face) < kStructureMinFaceSpanMm)
    {
        setReason("Face is too narrow when projected onto the build plane.");
        return false;
    }
    setReason("");
    return true;
}

namespace
{
/// True when `worldPoint` lies on `face`'s plane (within `planeTolM`) and inside its outer loop
/// (minus holes), projected into the face's own 2D basis. Used to attribute a carved fragment's
/// centroid back to the original face it came from: cutting only ever removes material, so a
/// fragment's centroid is always inside the original footprint it was cut from, no matter how far
/// it ends up from that face's overall centroid (centroid-distance matching breaks down badly on
/// lattice/strut patterns, where most fragments sit far from the panel's center).
/// Unsigned distance from `worldPoint` to `face`'s plane, or a negative value if `face` has no
/// usable plane. Two parallel panels at different heights can share a normal but must never be
/// matched to each other — plane distance alone already disambiguates that case, and disambiguates
/// it far more reliably than the 2D footprint test below (which can misfire on slivers whose
/// centroid lands exactly on a polygon edge).
double PlaneDistanceToFace(const Face *face, const glm::dvec3 &worldPoint)
{
    if (face == nullptr || face->surface == nullptr || face->loops.empty() || face->loops[0].empty())
        return -1.0;
    glm::dvec3 n = face->surface->GetNormal();
    const double nLen = glm::length(n);
    if (nLen < 1e-12)
        return -1.0;
    n /= nLen;
    const glm::dvec3 outer0 = face->loops[0][0].GetStartPosition();
    return std::abs(glm::dot(worldPoint - outer0, n));
}

/// True when `worldPoint`, already known to lie on `face`'s plane, projects inside its outer loop
/// (minus holes). Only used to break ties between multiple eligible faces sharing the same plane —
/// see `PlaneDistanceToFace`.
bool FaceContainsPointPlanar(const Face *face, const glm::dvec3 &worldPoint)
{
    if (face == nullptr || face->surface == nullptr || face->loops.empty())
        return false;
    glm::dvec3 n = face->surface->GetNormal();
    const double nLen = glm::length(n);
    if (nLen < 1e-12)
        return false;
    n /= nLen;

    // `loops[0]` is not reliably the outer boundary — STEP topology can store rings in either
    // order, and a slanted face's largest-area ring (the true outer) can land at any index.
    // `ClassifyOuterAndHoles` picks outer by area instead of trusting position 0.
    std::vector<std::vector<glm::dvec3>> rings;
    rings.reserve(face->loops.size());
    for (const auto &loop : face->loops)
    {
        std::vector<glm::dvec3> ring;
        ring.reserve(loop.size());
        for (const OrientedEdge &oe : loop)
            ring.push_back(oe.GetStartPosition());
        rings.push_back(std::move(ring));
    }
    std::vector<glm::dvec3> outer;
    std::vector<std::vector<glm::dvec3>> holes;
    GeometryOps::ClassifyOuterAndHoles(rings, n, outer, holes);
    if (outer.size() < 3)
        return false;

    glm::dvec3 u, v;
    GeometryOps::BuildPlanarBasis(n, u, v);
    const glm::dvec2 p2(glm::dot(worldPoint, u), glm::dot(worldPoint, v));
    return GeometryOps::InsideFootprintPlanar(p2, outer, holes, u, v);
}
} // namespace

void Display::RebuildStructureCarvedToOriginalMap()
{
    structureCarvedToOriginal.clear();
    if (!structureOriginalScene || scene == nullptr)
        return;

    constexpr double kPlaneTolM = 0.001; // 1mm: carving never moves material off-plane.
    size_t candidateCount = 0;
    size_t originalEligibleCount = 0;
    for (const Face &of : structureOriginalScene->faces)
        if (of.surface != nullptr && IsStructureFaceEligible(&of))
            ++originalEligibleCount;
    for (Face &cf : scene->faces)
    {
        // Deliberately not gated on `IsStructureFaceEligible(&cf)`: a carved fragment (strut top,
        // leftover sliver) is routinely smaller than `kMinFaceSpanMm` even though it belongs to a
        // perfectly eligible original panel. Only the *original* candidate's eligibility matters.
        if (cf.dependency == nullptr || cf.surface == nullptr)
            continue;
        ++candidateCount;
        const glm::dvec3 pC = StructureFaceCentroidWorld(&cf);

        // Primary key is plane distance, not the 2D footprint test: cutting never moves material
        // off-plane, so the correct original face is reliably whichever eligible candidate's plane
        // is closest — this is robust where the polygon test (loop winding/ordering, point landing
        // exactly on an edge for thin slivers) is not. The footprint test only breaks a genuine tie
        // between two eligible faces that share the same plane.
        std::vector<const Face *> onPlane;
        for (const Face &of : structureOriginalScene->faces)
        {
            if (of.surface == nullptr || !IsStructureFaceEligible(&of))
                continue;
            const double dist = PlaneDistanceToFace(&of, pC);
            if (dist >= 0.0 && dist <= kPlaneTolM)
                onPlane.push_back(&of);
        }
        const Face *match = nullptr;
        if (onPlane.size() == 1)
            match = onPlane.front();
        else if (onPlane.size() > 1)
        {
            for (const Face *of : onPlane)
                if (FaceContainsPointPlanar(of, pC))
                {
                    match = of;
                    break;
                }
            if (match == nullptr)
                match = onPlane.front();
        }
        if (match != nullptr)
            structureCarvedToOriginal.emplace(&cf, match);
    }
    LOG_SESSION("Structure carve->original map", "candidates=" + std::to_string(candidateCount),
             "original_eligible=" + std::to_string(originalEligibleCount),
             "matched=" + std::to_string(structureCarvedToOriginal.size()));
}

Display::StructurePickHit Display::PickStructureAtPixel(float pixelX, float pixelY) const
{
    StructurePickHit out;
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    glm::dvec3 ro, rd;
    ScenePick::OrthoPickRay(camera, w, h, pixelX, pixelY, ro, rd);
    out.face = ScenePick::PickClosestFace(renderer.GetPickTriangles(), ro, rd, PickFilter::Faces);
    if (out.face == nullptr)
        return out;
    // While staging, the hit is a carved fragment (a strut top, a leftover sliver, …) which can be
    // far smaller than `kMinFaceSpanMm` even though it belongs to a perfectly eligible original
    // panel — eligibility must be "does this map to an original panel", not the fragment's own size.
    if (IsStructureStagingActive())
        out.eligible = structureCarvedToOriginal.count(out.face) > 0;
    else
        out.eligible = IsStructureFaceEligible(out.face, &out.ineligibleReason);
    return out;
}

void Display::TryCommitStructureFacePick(float pixelX, float pixelY)
{
    if (activeTool != ActiveTool::Structure)
        return;
    if (structureOptFaceExcludeStep != Icons::StepState::Active)
        return;
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantCaptureMouse || HitTestUI(pixelX, pixelY) || HitTestImGui(pixelX, pixelY))
        return;
    if (GetActivePickFilter() == PickFilter::None)
        return;
    const StructurePickHit hit = PickStructureAtPixel(pixelX, pixelY);
    LOG_SESSION("Structure click",
             "face=" + std::to_string(reinterpret_cast<uintptr_t>(static_cast<const void *>(hit.face))),
             "eligible=" + std::to_string(hit.eligible),
             "staging=" + std::to_string(IsStructureStagingActive()));
    if (hit.face == nullptr)
        return;
    if (!hit.eligible)
    {
        // Clickability feedback is suppressed at this stage; silently ignore ineligible clicks.
        return;
    }

    // Pre-carve, `hit.face` is already an original face. While staging, resolve the carved-result
    // hit through the precomputed map; carve-only geometry (struts, cut boundaries) has no entry
    // and isn't a valid toggle target.
    const Face *toggleFace = hit.face;
    if (IsStructureStagingActive())
    {
        auto it = structureCarvedToOriginal.find(hit.face);
        if (it == structureCarvedToOriginal.end())
        {
            LOG_SESSION("Structure click: rejected, hit.face not in carvedToOriginal map");
            return;
        }
        toggleFace = it->second;
    }
    LOG_SESSION("Structure toggle",
             "toggleFace=" + std::to_string(reinterpret_cast<uintptr_t>(static_cast<const void *>(toggleFace))),
             "wasExcluded=" + std::to_string(structureExcludedFaces.count(toggleFace) > 0));
    if (structureExcludedFaces.count(toggleFace) > 0)
        structureExcludedFaces.erase(toggleFace);
    else
        structureExcludedFaces.insert(toggleFace);

    uiRenderer.MarkDirty();
    MarkPickDirty();
    if (IsStructureStagingActive())
        RebuildStructureStagingScene();
    else
    {
        RefreshStructurePreviewForRenderer();
        if (pendingStructureStagingTask.has_value())
            BeginStructureStagingSession();
    }
}

void Display::ClearStructureFacePick()
{
    if (structureExcludedFaces.empty())
        return;
    structureExcludedFaces.clear();
    structureEligibleFacesCache.clear();
    uiRenderer.MarkDirty();
    MarkPickDirty();
}

void Display::RefreshCalibWorkflow()
{
    if (!scene)
    {
        calibWorkflow = CalibWorkflow::None;
        RefreshCalibCompensation();
        RefreshCalibDerivedRowVisible();
        return;
    }
    const glm::dvec3 calibBuildDir = CalibrateDistance::DefaultCalibrateBuildDirection();
    const double layerMm = static_cast<double>(layerHeight);
    const Face *f1 = ResolveCalibFaceForWorkflow(calibFacePoint1, calibEdgePoint1);
    const Face *f2 = ResolveCalibFaceForWorkflow(calibFacePoint2, calibEdgePoint2);
    std::unordered_set<const Edge *> layerHoleInnerEdges;
    if (CalibSlotHasPick(calibFacePoint1, calibEdgePoint1) || CalibSlotHasPick(calibFacePoint2, calibEdgePoint2))
        GatherCalibLayerHoleInnerEdges(scene, calibBuildDir, layerHoleInnerEdges);

    const bool elephantFootEdges =
        calibFacePoint1 != nullptr && calibFacePoint1 == calibFacePoint2 && calibEdgePoint1 != nullptr &&
        calibEdgePoint2 != nullptr && calibEdgePoint1 != calibEdgePoint2 &&
        CalibrateDistance::FaceInFirstLayerSlab(calibFacePoint1, scene, layerMm, calibBuildDir) &&
        CalibrateDistance::FaceIsLayerCapParallelBuild(calibFacePoint1, calibBuildDir) &&
        CalibrateNominal::EdgeBelongsToFace(calibEdgePoint1, calibFacePoint1) &&
        CalibrateNominal::EdgeBelongsToFace(calibEdgePoint2, calibFacePoint1) &&
        CalibrateNominal::EdgesAreParallelForCalib(calibEdgePoint1, calibEdgePoint2);

    if (elephantFootEdges)
        calibWorkflow = CalibWorkflow::ElephantFoot;
    else if (f1 != nullptr && f2 != nullptr)
    {
        calibWorkflow =
            CalibrateDistance::CombinePickedFaces(f1, f2, scene, layerMm, calibBuildDir, layerHoleInnerEdges);
        if (calibWorkflow == CalibWorkflow::Contour || calibWorkflow == CalibWorkflow::Hole)
        {
            if (!CalibrateNominal::NominalSpanPerpendicularToBuild(f1, f2, calibBuildDir))
                calibWorkflow = CalibWorkflow::None;
        }
    }
    else if (f1 != nullptr)
        calibWorkflow =
            CalibrateDistance::ClassifyFace(f1, scene, layerMm, calibBuildDir, layerHoleInnerEdges);
    else if (f2 != nullptr)
        calibWorkflow =
            CalibrateDistance::ClassifyFace(f2, scene, layerMm, calibBuildDir, layerHoleInnerEdges);
    else
        calibWorkflow = CalibWorkflow::None;

    RefreshCalibCompensation();
    RefreshCalibDerivedRowVisible();
}

void Display::RefreshCalibDerivedRowVisible()
{
    if (!calibPara_Derived)
        return;

    bool next = false;
    const bool parameterRowsVisible = calibSec_Parameters ? calibSec_Parameters->visible
                                                          : (calibPara_Measure && calibPara_Measure->visible);
    if (parameterRowsVisible)
    {
        const bool importDone = ImportAllowsGeometryDependentTools();
        const bool hasTwoPicks = CalibSlotHasPick(calibFacePoint1, calibEdgePoint1) &&
                                 CalibSlotHasPick(calibFacePoint2, calibEdgePoint2);
        next = importDone && hasTwoPicks;
    }

    bool changedVis = false;
    if (calibPara_Derived->visible != next)
    {
        calibPara_Derived->visible = next;
        changedVis = true;
    }
    if (calibSec_Result && calibSec_Result->visible != next)
    {
        calibSec_Result->visible = next;
        changedVis = true;
    }
    if (changedVis)
        uiRenderer.MarkDirty();
}

void Display::RefreshCalibCompensation()
{
    const bool prevValid = calibCompensationValid;
    const std::optional<ToolUserErrorPayload> prevErr = calibToolError;

    calibContourScale = 1.0f;
    calibHoleOffsetMm = 0.0f;
    calibElephantFootMm = 0.0f;
    calibCompensationValid = false;
    calibNominal = 0.0f;
    calibToolError.reset();

    auto setPickError = [this](const char *code, const char *msg)
    {
        calibToolError.emplace(ToolUserErrorPayload{std::string(code), std::string(msg),
                                                    std::string(kCalibPlotMeasurementPointsLabel)});
    };

    const Face *spanA = ResolveCalibFaceForWorkflow(calibFacePoint1, calibEdgePoint1);
    const Face *spanB = ResolveCalibFaceForWorkflow(calibFacePoint2, calibEdgePoint2);
    if (scene == nullptr || spanA == nullptr || spanB == nullptr)
    {
        if (prevValid != calibCompensationValid || prevErr != calibToolError)
            uiRenderer.MarkDirty();
        return;
    }

    CalibrateNominal::SpanResult span;
    if (calibWorkflow == CalibWorkflow::ElephantFoot && calibEdgePoint1 != nullptr &&
        calibEdgePoint2 != nullptr && spanA != nullptr && spanA == spanB)
        span = CalibrateNominal::SpanBetweenParallelEdgesOnFace(spanA, calibEdgePoint1, calibEdgePoint2);
    else
        span = CalibrateNominal::SpanBetweenFaces(spanA, spanB);
    if (!span.valid)
    {
        setPickError("CAL_SPAN_GEOMETRY", "Could not estimate a CAD span from the current face picks.");
        if (prevValid != calibCompensationValid || prevErr != calibToolError)
            uiRenderer.MarkDirty();
        return;
    }

    if (calibWorkflow != CalibWorkflow::ElephantFoot &&
        !CalibrateNominal::NominalSpanPerpendicularToBuild(spanA, spanB, CalibrateDistance::DefaultCalibrateBuildDirection()))
    {
        setPickError("CAL_SPAN_AXIS",
                     "CAD span from the picks is not usable with the default build axis for this workflow.");
        if (prevValid != calibCompensationValid || prevErr != calibToolError)
            uiRenderer.MarkDirty();
        return;
    }

    calibNominal = span.nominalMm;
    if (calibNominal <= 1e-5f)
    {
        setPickError("CAL_SPAN_TINY", "CAD span is effectively zero; try parallel faces or a longer span.");
        if (prevValid != calibCompensationValid || prevErr != calibToolError)
            uiRenderer.MarkDirty();
        return;
    }

    if (calibWorkflow == CalibWorkflow::None)
    {
        setPickError("CAL_WORKFLOW_UNSUPPORTED",
                     "The two picks do not support a supported calibration workflow together.");
        if (prevValid != calibCompensationValid || prevErr != calibToolError)
            uiRenderer.MarkDirty();
        return;
    }

    const CalibrateCompensation::Values vals =
        CalibrateCompensation::Compute(calibWorkflow, calibNominal, calibMeasured);
    if (!vals.valid)
    {
        if (vals.errorCode && vals.errorMessage && vals.errorParameterLabel)
            calibToolError.emplace(vals.errorCode, vals.errorMessage, vals.errorParameterLabel);
        if (prevValid != calibCompensationValid || prevErr != calibToolError)
            uiRenderer.MarkDirty();
        return;
    }

    calibContourScale = vals.contourScale;
    calibHoleOffsetMm = vals.holeRadiusOffsetMm;
    calibElephantFootMm = vals.elephantFootExcessMm;
    calibCompensationValid = true;

    if (prevValid != calibCompensationValid || prevErr != calibToolError)
        uiRenderer.MarkDirty();
}

void Display::snapInput(float &x, float &y)
{
    if (std::hypot(x, y) < kPanSnapTravelFloor)
        return;
    if (std::abs(x) <= std::abs(y) * 0.5f)
        x = 0;
    else if (std::abs(y) <= std::abs(x) * 0.5f)
        y = 0;
}

void Display::BeginOrbitSnapGesture()
{
    orbitSnapGestureActive = true;
    orbitSnapSuppressedAxis = camera.PrincipalSnapAxis(UserTuning::snapEnterDeg);
}

void Display::Orbit(float offsetX, float offsetY)
{
    camera.Orbit(offsetX, offsetY);
    if (orbitSnapGestureActive)
    {
        const float liveSnapDeg = std::max(UserTuning::snapEnterDeg * 2.2f, UserTuning::snapEnterDeg + 2.0f);
        const float releaseDeg = liveSnapDeg + 1.5f;
        if (orbitSnapSuppressedAxis != Camera::OrbitSnapAxis::None &&
            !camera.IsWithinSnapAxis(orbitSnapSuppressedAxis, releaseDeg))
        {
            orbitSnapSuppressedAxis = Camera::OrbitSnapAxis::None;
        }
        camera.SnapToPrincipalAxis(liveSnapDeg, orbitSnapSuppressedAxis);
    }

    UpdateCamera();
}

void Display::FinishOrbitSnap()
{
    camera.FinishOrbitSnap(orbitSnapSuppressedAxis);
    orbitSnapGestureActive = false;
    orbitSnapSuppressedAxis = Camera::OrbitSnapAxis::None;
    UpdateCamera();
}

void Display::Roll(float delta)
{
    camera.Roll(delta);
    UpdateCamera();
}

void Display::Pan(float offsetX, float offsetY, bool scroll)
{
    snapInput(offsetX, offsetY);
    camera.Pan(offsetX, offsetY, scroll);

    UpdateCamera();
}

void Display::FrameScene()
{
    if (scene->points.empty())
        return;

    glm::vec3 min(std::numeric_limits<float>::max());
    glm::vec3 max(std::numeric_limits<float>::lowest());

    for (const auto &point : scene->points)
    {
        glm::vec3 pos(point.position);
        min = glm::min(min, pos);
        max = glm::max(max, pos);
    }

    camera.FrameBounds(min, max);
    UpdateCamera();
}

void Display::ResetCameraView()
{
    camera.ResetHomeView();
    UpdateCamera();
}

bool Display::HitTestUI(float pixelX, float pixelY) const
{
    return uiRenderer.HitTest(pixelX, pixelY);
}

bool Display::HitTestImGui(float pixelX, float pixelY) const
{
    ImGuiContext *ctx = ImGui::GetCurrentContext();
    if (ctx == nullptr)
        return false;
    ImGuiWindow *hovered = nullptr;
    ImGuiWindow *hoveredUnderMoving = nullptr;
    ImGui::FindHoveredWindowEx(ImVec2(pixelX, pixelY), false, &hovered, &hoveredUnderMoving);
    return hovered != nullptr;
}

void Display::MarkBug()
{
    auto &sl = SessionLogger::Instance();
    FillSessionReproState(sl.state);
    sl.LogBugMarker();
}

void Display::FillSessionReproState(SessionState &s) const
{
    if (scene != nullptr)
    {
        s.points = scene->points.size();
        s.edges = scene->edges.size();
        s.faces = scene->faces.size();
        s.solids = scene->solids.size();
    }

    s.overhangAngle = overhangAngle;
    s.sharpCornerThreshold = sharpCornerThreshold;
    s.instabilityMinWidth = instabilityMinWidth;
    s.layerDifferenceMaxAreaDelta = layerDifferenceMaxAreaDelta;
    s.layerHeight = layerHeight;

    s.cameraTarget = camera.target;
    s.cameraOrthoSize = camera.orthoSize;
    s.cameraPosition = camera.GetPosition();
    s.cameraDistance = camera.distance;
    s.cameraQuatW = camera.orientation.w;
    s.cameraQuatX = camera.orientation.x;
    s.cameraQuatY = camera.orientation.y;
    s.cameraQuatZ = camera.orientation.z;
    s.cameraNearPlane = camera.nearPlane;
    s.cameraFarPlane = camera.farPlane;

    s.windowLogicalW = static_cast<int>(windowWidth);
    s.windowLogicalH = static_cast<int>(windowHeight);
    if (window != nullptr)
        SDL_GetWindowSizeInPixels(window, &s.windowPixelsW, &s.windowPixelsH);
    else
    {
        s.windowPixelsW = 0;
        s.windowPixelsH = 0;
    }

    if (activeTool == ActiveTool::Calibrate)
        s.activeToolOrdinal = 1u;
    else if (activeTool == ActiveTool::Structure)
        s.activeToolOrdinal = 2u;
    else
        s.activeToolOrdinal = 0u;
    s.viewportAnalysisEnabled = analysisEnabled;
    s.depthExperimentOrdinal = static_cast<uint8_t>(ViewportDepthExperiments::Active());
}

void Display::SyncToolbarToolVisualState()
{
    if (toolbarAnalysisLine && toolbarCalibrateLine && toolbarStructureLine && uiAnalysis && uiCalibrate &&
        uiStructure)
    {
        toolbarAnalysisLine->selected =
            (activeTool == ActiveTool::Analysis && uiAnalysis->visible);
        toolbarCalibrateLine->selected =
            (activeTool == ActiveTool::Calibrate && uiCalibrate->visible);
        toolbarStructureLine->selected =
            (activeTool == ActiveTool::Structure && uiStructure->visible);
    }
    uiRenderer.MarkDirty();
}

void Display::PublishImportProgress(uint64_t generation, const ImportProgress &progress)
{
    if (generation != importProgressGeneration.load(std::memory_order_relaxed))
        return;

    std::lock_guard<std::mutex> lock(importProgressMutex);
    if (generation != importProgressGeneration.load(std::memory_order_relaxed))
        return;

    latestImportProgressPhase = progress.phase;
    latestImportProgress01 = progress.progress01;
    latestImportProgressDirty = true;
}

void Display::ApplyImportProgressSnapshot()
{
    std::string phase;
    float progress01 = -1.0f;
    {
        std::unique_lock<std::mutex> lock(importProgressMutex, std::try_to_lock);
        if (!lock.owns_lock())
            return;
        if (!latestImportProgressDirty)
            return;

        phase = latestImportProgressPhase;
        progress01 = latestImportProgress01;
        latestImportProgressDirty = false;
    }

    SetImportProgress(std::move(phase), progress01);
}

void Display::ClearPendingImportProgressSnapshot()
{
    std::unique_lock<std::mutex> lock(importProgressMutex, std::try_to_lock);
    if (!lock.owns_lock())
        return;
    latestImportProgressPhase.clear();
    latestImportProgress01 = -1.0f;
    latestImportProgressDirty = false;
}

void Display::SetImportProgress(std::string phase, float progress01)
{
    importProgressPhase = std::move(phase);
    importProgress01 =
        (progress01 >= 0.0f && progress01 <= 1.0f) ? std::clamp(progress01, 0.0f, 1.0f) : -1.0f;
    uiRenderer.MarkDirty();
}

void Display::PublishStructureCarveProgress(uint64_t jobId, const ImportProgress &progress)
{
    std::lock_guard<std::mutex> lock(structureProgressMutex);
    if (jobId != structureProgressJobId)
        return;

    latestStructureProgressPhase = progress.phase;
    latestStructureProgress01 = progress.progress01;
    latestStructureProgressDirty = true;
}

void Display::ApplyStructureProgressSnapshot()
{
    std::string phase;
    float progress01 = -1.0f;
    {
        std::unique_lock<std::mutex> lock(structureProgressMutex, std::try_to_lock);
        if (!lock.owns_lock())
            return;
        if (!latestStructureProgressDirty)
            return;

        phase = latestStructureProgressPhase;
        progress01 = latestStructureProgress01;
        latestStructureProgressDirty = false;
    }

    structureProgressPhase = std::move(phase);
    structureProgress01 =
        (progress01 >= 0.0f && progress01 <= 1.0f) ? std::clamp(progress01, 0.0f, 1.0f) : -1.0f;
    uiRenderer.MarkDirty();
}

void Display::ClearPendingStructureProgressSnapshot()
{
    std::unique_lock<std::mutex> lock(structureProgressMutex, std::try_to_lock);
    if (!lock.owns_lock())
        return;
    latestStructureProgressPhase.clear();
    latestStructureProgress01 = -1.0f;
    latestStructureProgressDirty = false;
    structureProgressPhase.clear();
    structureProgress01 = -1.0f;
}

void Display::RefreshToolProcessingCards(bool hasModel, bool geometryOrStyleWork, bool ranMainThreadApplyTask)
{
    bool changed = false;
    auto setVisible = [&](Paragraph *p, bool visible)
    {
        if (!p)
            return;
        if (p->visible != visible)
        {
            p->visible = visible;
            changed = true;
        }
    };
    auto setBool = [&](bool &dst, bool value)
    {
        if (dst != value)
        {
            dst = value;
            changed = true;
        }
    };
    auto setFloat = [&](float &dst, float value)
    {
        if (std::fabs(dst - value) > 1e-6f)
        {
            dst = value;
            changed = true;
        }
    };
    auto setText = [&](Paragraph *p, const std::string &text)
    {
        if (!p || p->values.empty())
            return;
        if (p->values[0].text != text)
        {
            p->values[0].text = text;
            changed = true;
        }
    };
    auto ensureLine = [&](Paragraph *p, std::size_t lineIndex) -> SectionLine *
    {
        if (!p)
            return nullptr;
        while (p->values.size() <= lineIndex)
        {
            SectionLine &line = p->values.emplace_back();
            line.fontScale = 0.85f;
            line.textDepth = 1;
            line.onClick = p->onClick;
            changed = true;
        }
        return &p->values[lineIndex];
    };
    auto setLineText = [&](Paragraph *p, std::size_t lineIndex, const std::string &text)
    {
        SectionLine *line = ensureLine(p, lineIndex);
        if (!line)
            return;
        if (line->text != text)
        {
            line->text = text;
            changed = true;
        }
    };
    auto setLineVisible = [&](Paragraph *p, std::size_t lineIndex, bool visible)
    {
        SectionLine *line = ensureLine(p, lineIndex);
        if (!line)
            return;
        if (line->visible != visible)
        {
            line->visible = visible;
            changed = true;
        }
    };
    auto setAccentCounts = [&](Paragraph *p, int64_t num, int64_t den)
    {
        if (!p)
            return;
        if (p->accentProgressNumerator != num || p->accentProgressDenominator != den || p->accentProgress01 >= 0.0f)
        {
            p->accentProgressNumerator = num;
            p->accentProgressDenominator = den;
            p->accentProgress01 = -1.0f;
            changed = true;
        }
    };
    auto clearAccentCounts = [&](Paragraph *p)
    {
        if (!p)
            return;
        if (p->accentProgressNumerator != -1 || p->accentProgressDenominator != -1)
        {
            p->accentProgressNumerator = -1;
            p->accentProgressDenominator = -1;
            changed = true;
        }
    };

    const bool activeHasModel = hasModel && !pendingImportTabActive;
    const bool importActive = importBusy || pendingImportTask.has_value();
    const bool activePendingImport = pendingImportTabActive && importActive;
    const bool hasCommittedAnalysis = lastCommittedAnalysisForRecolor.has_value();
    const bool queueingFirstAnalysis = pendingAnalysisAfterGeometryRebuild && !hasCommittedAnalysis;
    const bool pendingTintThisScene =
        pendingAnalysisTint.has_value() && pendingAnalysisTint->scene == scene;
    const bool analysisRenderingInScene =
        analysisEnabled && activeAnalysisTintForRebuild.has_value() && renderer.FullRebuildInProgress();
    // Hide verdict/counts only while a worker run is in flight or a completed run is still queued for tint apply.
    // Do NOT include queueingFirstAnalysis here: it stays true until geometry rebuild commits recolor, which is
    // the same window as GPU incremental rebuild — hiding panels on it kept counts/verdict off after results exist.
    const bool analysisPipelineWaiting =
        analysisEnabled && (pendingAnalysisTask.has_value() || pendingTintThisScene);
    // Processing card + bar: full pipeline including "waiting to launch" and GPU tint application.
    const bool analysisBusy =
        analysisEnabled &&
        (queueingFirstAnalysis || analysisPipelineWaiting || analysisRenderingInScene);

    if (!analysisBusy)
    {
        ++analysisProcessingIdleStreak;
        if (analysisProcessingIdleStreak >= 3)
            ResetAnalysisPipelineTotals();
    }
    else
    {
        analysisProcessingIdleStreak = 0;

        if (activeHasModel && scene != nullptr)
            RefreshAnalysisPipelineDenominatorFromScene();

        if (analysisRenderingInScene && renderer.FullRebuildInProgress())
            analysisGpuRebuildStepsCache = renderer.IncrementalRebuildStepsDone();

        const char *phaseTitle = "Working on analysis...";
        if (queueingFirstAnalysis)
            phaseTitle = "Queueing analysis...";
        else if (pendingAnalysisTask.has_value())
            phaseTitle = AnalysisWorkerPhaseTitle(analysisWorkerPhaseIdAtomic.load(std::memory_order_relaxed));
        else if (pendingTintThisScene)
            phaseTitle = "Applying analysis...";
        else if (analysisRenderingInScene)
            phaseTitle = "Rendering analysis...";

        uint64_t numerator = analysisWorkerStepsDone.load(std::memory_order_relaxed);
        numerator += analysisTintStepsDone.load(std::memory_order_relaxed);
        if (analysisRenderingInScene)
            numerator += analysisGpuRebuildStepsCache;

        uint64_t denominator = analysisPipelineDenomTotal;
        if (denominator > 0)
            numerator = std::min(numerator, denominator);

        if (uiAnalysisProcessing)
        {
            setText(uiAnalysisProcessing, phaseTitle);
            if (denominator > 0)
                setAccentCounts(uiAnalysisProcessing, static_cast<int64_t>(numerator), static_cast<int64_t>(denominator));
            else
            {
                clearAccentCounts(uiAnalysisProcessing);
                setFloat(uiAnalysisProcessing->accentProgress01, -1.0f);
            }
        }
    }

    if (uiAnalysisProcessing)
    {
        setVisible(uiAnalysisProcessing, analysisBusy && activeHasModel);
        setBool(uiAnalysisProcessing->accentProgressBar, uiAnalysisProcessing->visible);
        if (!uiAnalysisProcessing->visible)
        {
            clearAccentCounts(uiAnalysisProcessing);
            setFloat(uiAnalysisProcessing->accentProgress01, -1.0f);
        }
    }

    // Import prerequisite row: mirror Files-tab import progress on the Analysis "Import a file" step.
    if (uiImportPara)
    {
        setVisible(uiImportPara, activePendingImport || !activeHasModel);
        setBool(uiImportPara->accentProgressBar, activePendingImport);
        const std::string importPhase =
            importProgressPhase.empty() ? std::string("Importing file...") : importProgressPhase;
        setText(uiImportPara, activePendingImport ? ImportPrerequisiteTitle(importProgress01) : "Import a file");
        setLineText(uiImportPara, 1, activePendingImport ? importPhase : "");
        setLineVisible(uiImportPara, 1, activePendingImport);
        if (activePendingImport)
            setFloat(uiImportPara->accentProgress01,
                     importProgress01 >= 0.0f ? importProgress01 : -1.0f);
        else
            setFloat(uiImportPara->accentProgress01, -1.0f);
    }

    if (uiResult)
        setVisible(uiResult, activeHasModel && analysisUiScene == scene);
    if (uiVerdict)
        setVisible(uiVerdict, activeHasModel && analysisUiScene == scene);

    // Import progress lives in the Files bar tab; keep Calibrate panel unobstructed during import.
    const bool calibrateBusy =
        activeHasModel && (geometryOrStyleWork || ranMainThreadApplyTask || renderer.FullRebuildInProgress());
    if (uiCalibrateProcessing)
    {
        const bool show = calibrateBusy && uiCalibrate && uiCalibrate->visible;
        setVisible(uiCalibrateProcessing, show);
        setBool(uiCalibrateProcessing->accentProgressBar, show);
        setFloat(uiCalibrateProcessing->accentProgress01, importProgress01 >= 0.0f ? importProgress01 : -1.0f);
        if (show)
        {
            const std::string importPhase =
                importProgressPhase.empty() ? std::string("Importing model...") : importProgressPhase;
            setText(uiCalibrateProcessing,
                    importBusy ? ImportProgressLabel(importPhase, importProgress01) : "Refreshing calibration...");
            if (!importBusy && uiCalibrateProcessing->accentProgress01 < 0.0f)
                setFloat(uiCalibrateProcessing->accentProgress01, 0.75f);
        }
    }

    const bool structureCarveBusy =
        activeHasModel && (structureCarvePipelinePhase != StructureCarvePipelinePhase::Idle ||
                           pendingStructureStagingTask.has_value());
    if (uiStructure && uiStructure->header.has_value())
    {
        Paragraph *hpara = &uiStructure->header->para;
        const bool show = structureCarveBusy && uiStructure->visible;
        setBool(hpara->accentProgressBar, show);
        const uint64_t solidsTotal = structureStagingStepsTotal;
        const uint64_t solidsDone =
            std::min(structureStagingStepsDone.load(std::memory_order_relaxed), std::max<uint64_t>(solidsTotal, 1));
        if (show && structureProgress01 >= 0.0f)
        {
            // Fine-grained within-solid progress (per-face carve + unify/rebuild checkpoints) from
            // StructureCarve::TryApplyStructureCarve, blended with the already-finished solid count
            // so multi-solid jobs still read as one continuous bar across solids.
            clearAccentCounts(hpara);
            const float perSolidSpan = solidsTotal > 0 ? 1.0f / static_cast<float>(solidsTotal) : 1.0f;
            const float overall = (static_cast<float>(solidsDone) + structureProgress01) * perSolidSpan;
            setFloat(hpara->accentProgress01, std::clamp(overall, 0.0f, 1.0f));
        }
        else if (show && solidsTotal > 0)
        {
            setAccentCounts(hpara, static_cast<int64_t>(solidsDone), static_cast<int64_t>(solidsTotal));
        }
        else
        {
            clearAccentCounts(hpara);
            setFloat(hpara->accentProgress01, -1.0f); // indeterminate fallback
        }
    }

    if (calibSec_Prerequisites)
    {
        if (calibSec_Prerequisites->visible != !calibrateBusy)
        {
            calibSec_Prerequisites->visible = !calibrateBusy;
            changed = true;
        }
    }
    if (calibSec_Parameters)
    {
        const bool nextVisible = !calibrateBusy && ImportAllowsGeometryDependentTools();
        if (calibSec_Parameters->visible != nextVisible)
        {
            calibSec_Parameters->visible = nextVisible;
            changed = true;
        }
    }
    if (calibPara_Measure)
    {
        const bool nextVisible = !calibrateBusy && ImportAllowsGeometryDependentTools();
        if (calibPara_Measure->visible != nextVisible)
        {
            calibPara_Measure->visible = nextVisible;
            changed = true;
        }
    }
    RefreshCalibDerivedRowVisible();
    if (changed)
        uiRenderer.MarkDirty();
}

void Display::FlushImportInputEventTail()
{
    SDL_FlushEvents(SDL_EVENT_FINGER_DOWN, SDL_EVENT_FINGER_CANCELED);
    SDL_FlushEvents(SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_WHEEL);
    if (inputForGestureSync != nullptr)
        inputForGestureSync->NotifySdlEventQueueFlushed();
}

std::chrono::milliseconds Display::WorkerFuturePollRemainingMs() const
{
    if (!workerFuturePollDeadline.has_value())
        return std::chrono::milliseconds(0);
    const auto now = std::chrono::steady_clock::now();
    if (now >= *workerFuturePollDeadline)
        return std::chrono::milliseconds(0);
    return std::chrono::duration_cast<std::chrono::milliseconds>(*workerFuturePollDeadline - now);
}

void Display::ProcessDeferredImportIfAny()
{
    using namespace std::chrono_literals;

    if (!taskRunner)
        return;

    if (pendingImportTask.has_value())
    {
        std::optional<AsyncImportResult> readyResult = pendingImportTask->TryTake(WorkerFuturePollRemainingMs());
        if (!readyResult.has_value())
            return;
        AsyncImportResult result = std::move(*readyResult);
        pendingImportTask.reset();

        if (!result.ok || !result.importedScene)
        {
            if (!result.cancelled)
                LOG_WARN("Import failed for", result.path);
            importBusy = false;
            importProgress01 = -1.0f;
            importProgressPhase.clear();
            ClearPendingImportProgressSnapshot();
            pendingImportTabStem.clear();
            pendingImportTabActive = false;
            pendingFileTabsRebuild = true;
            return;
        }

        struct ImportApplyState
        {
            AsyncImportResult result;
            std::string filename;
            size_t importedSceneIndex = SIZE_MAX;
            bool activateImportedScene = false;
            double frameSceneMs = 0.0;
            double updateSceneMs = 0.0;
        };

        auto state = std::make_shared<ImportApplyState>();
        state->result = std::move(result);
        state->filename = state->result.path.substr(state->result.path.find_last_of("/\\") + 1);

        mainThreadPipeline.Enqueue("import-attach-scene", [this, state](double) -> bool
                                   {
                                       SetImportProgress("Attaching imported scene...", 0.85f);
                                       ownedScenes.push_back(std::move(state->result.importedScene));
                                       if (Scene *attached = ownedScenes.back().get())
                                           attached->TryCreateCompoundWrappingAllSolidsIfNone();
                                       state->importedSceneIndex = ownedScenes.size() - 1;
                                       state->activateImportedScene = pendingImportTabActive;
                                       if (state->activateImportedScene)
                                       {
                                           activeSceneIndex = state->importedSceneIndex;
                                           scene = ownedScenes.back().get();
                                           analysisUiScene = nullptr;
                                           lastVerdictWasPass = false;
                                           flawOverhang = {};
                                           flawSharpCorner = {};
                                           flawInstability = {};
                                           flawLayerDifference = {};
                                           if (uiVerdict)
                                               uiVerdict->values.clear();
                                           skipAnalysisForNextGeometryRebuild = true;
                                       }
                                       return true;
                                   });

        mainThreadPipeline.Enqueue("import-frame-scene", [this, state](double) -> bool
                                   {
                                       SetImportProgress("Framing imported scene...", 0.90f);
                                       if (!state->activateImportedScene)
                                           return true;
                                       using Clock = std::chrono::steady_clock;
                                       const Clock::time_point tStart = Clock::now();
                                       FrameScene();
                                       state->frameSceneMs =
                                           std::chrono::duration<double, std::milli>(Clock::now() - tStart).count();
                                       return true;
                                   });

        mainThreadPipeline.Enqueue("import-update-scene", [this, state](double) -> bool
                                   {
                                       SetImportProgress("Refreshing viewport data...", 0.95f);
                                       if (!state->activateImportedScene)
                                           return true;
                                       using Clock = std::chrono::steady_clock;
                                       const Clock::time_point tStart = Clock::now();
                                       UpdateScene();
                                       state->updateSceneMs =
                                           std::chrono::duration<double, std::milli>(Clock::now() - tStart).count();
                                       return true;
                                   });

        mainThreadPipeline.Enqueue("import-finalize-ui", [this, state](double) -> bool
                                   {
                                       SetImportProgress("Finalizing import...", 1.0f);
                                       if (state->result.lower == "stl" && state->result.hasStlStats)
                                       {
                                           const double pipelineMs =
                                               state->result.importerMs + state->frameSceneMs + state->updateSceneMs;
                                           LOG_INFO("Import timing STL parseMs", state->result.stlParseMs,
                                                    "mergeMs", state->result.stlMergeMs,
                                                    "stlTotalMs", state->result.stlTotalMs,
                                                    "importerMs", state->result.importerMs,
                                                    "frameSceneMs", state->frameSceneMs,
                                                    "updateSceneMs", state->updateSceneMs,
                                                    "pipelineMs", pipelineMs,
                                                    "triangles", state->result.stlTriangles,
                                                    "uniquePoints", state->result.stlUniquePoints,
                                                    "faces", state->result.stlFaces);
                                       }

                                       auto &sl = SessionLogger::Instance();
                                       Scene *importedScene = state->importedSceneIndex < ownedScenes.size()
                                                                  ? ownedScenes[state->importedSceneIndex].get()
                                                                  : scene;
                                       sl.state.lastFilename = state->filename;
                                       sl.state.lastFormat = state->result.lower;
                                       sl.state.points = importedScene ? importedScene->points.size() : 0;
                                       sl.state.edges = importedScene ? importedScene->edges.size() : 0;
                                       sl.state.faces = importedScene ? importedScene->faces.size() : 0;
                                       sl.state.solids = importedScene ? importedScene->solids.size() : 0;
                                       sl.LogFileImport(state->filename, state->result.lower);

                                       if (state->result.lower == "stl" && state->result.hasStlMergeDiagnostics)
                                       {
                                           STLImportStats stlReplay;
                                           stlReplay.isBinary = state->result.stlIsBinary;
                                           stlReplay.triangleCount = state->result.stlTriangles;
                                           stlReplay.uniquePoints = state->result.stlUniquePoints;
                                           stlReplay.faces = state->result.stlFaces;
                                           stlReplay.parseMs = state->result.stlParseMs;
                                           stlReplay.mergeMs = state->result.stlMergeMs;
                                           stlReplay.totalMs = state->result.stlTotalMs;
                                           stlReplay.hasMergeDiagnostics = true;
                                           stlReplay.mergeDiagnostics = state->result.stlMergeDiagnostics;
                                           sl.LogStlMergeDiagnostics(state->filename, stlReplay);
                                       }

                                       pendingImportTabStem.clear();
                                       pendingImportTabActive = false;
                                       openFiles.push_back(state->filename);
                                       RebuildFileTabs();

                                       if (state->activateImportedScene)
                                       {
                                           importOpenBoundaryBannerDismissed = false;
                                           calibStepImport = Icons::StepState::Done;
                                           calibPara_Import->visible = false;
                                           RefreshImportClosedVolumeContractFromScene();
                                           const bool toolsReady = ImportAllowsGeometryDependentTools();
                                           calibPara_Point1->visible = toolsReady;
                                           calibPara_Point2->visible = toolsReady;
                                           if (calibSec_Parameters)
                                               calibSec_Parameters->visible = toolsReady;
                                           if (calibPara_Measure)
                                               calibPara_Measure->visible = toolsReady;
                                           SyncStructurePanelDerivedVisibility();
                                           ClearCalibrateFacePicks();
                                           SyncCalibrateImportPrerequisiteVisibility();
                                           if (activeTool == ActiveTool::Structure)
                                           {
                                               if (IsStructureStagingActive())
                                                   RestoreStructureOriginalScene();
                                               BeginStructureStagingSession();
                                           }
                                       }
                                       uiRenderer.MarkDirty();
                                       renderDirty = true;

                                       FlushImportInputEventTail();

                                       importBusy = false;
                                       importProgress01 = -1.0f;
                                       importProgressPhase.clear();
                                       ClearPendingImportProgressSnapshot();
                                       return true;
                                   });
        return;
    }

    if (!deferredImportPath)
        return;
    std::string path = std::move(*deferredImportPath);
    deferredImportPath.reset();

    const std::string fname = std::filesystem::path(path).filename().string();
    pendingImportTabStem = std::filesystem::path(path).stem().string();
    pendingImportTabActive = true;
    importBusy = true;
    ClearPendingImportProgressSnapshot();
    const uint64_t importGeneration = importProgressGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    SetImportProgress("Opening file...", 0.0f);
    pendingFileTabsRebuild = true;
    Render();

    mainThreadPipeline.Clear();

    if (pendingImportTask.has_value())
    {
        pendingImportTask->RequestCancel();
        pendingImportTask.reset();
    }

    pendingImportTask = taskRunner->Submit([this, path, importGeneration](const TaskRunner::CancellationToken &token) -> AsyncImportResult
                                          {
                                              using Clock = std::chrono::steady_clock;
                                              AsyncImportResult result;
                                              result.path = path;
                                              ImportProgressCallback importProgress = [this, importGeneration](const ImportProgress &progress)
                                              {
                                                  PublishImportProgress(importGeneration, progress);
                                              };

                                              auto ext = path.substr(path.find_last_of('.') + 1);
                                              for (char c : ext)
                                                  result.lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                                              if (token.IsCancellationRequested())
                                              {
                                                  result.cancelled = true;
                                                  return result;
                                              }

                                              result.importedScene = std::make_unique<Scene>();
                                              const Clock::time_point tImporterStart = Clock::now();

                                              if (result.lower == "stl")
                                              {
                                                  STLImportStats stlStats;
                                                  result.ok = STLImport::Import(path, result.importedScene.get(), &stlStats, &importProgress);
                                                  result.hasStlStats = result.ok;
                                                  if (result.hasStlStats)
                                                  {
                                                      result.stlParseMs = stlStats.parseMs;
                                                      result.stlMergeMs = stlStats.mergeMs;
                                                      result.stlTotalMs = stlStats.totalMs;
                                                      result.stlTriangles = stlStats.triangleCount;
                                                      result.stlUniquePoints = stlStats.uniquePoints;
                                                      result.stlFaces = stlStats.faces;
                                                      result.stlIsBinary = stlStats.isBinary;
                                                      result.hasStlMergeDiagnostics = stlStats.hasMergeDiagnostics;
                                                      result.stlMergeDiagnostics = stlStats.mergeDiagnostics;
                                                  }
                                              }
                                              else if (result.lower == "obj")
                                              {
                                                  result.ok = OBJImport::Import(path, result.importedScene.get(), &importProgress);
                                              }
                                              else if (result.lower == "3mf")
                                              {
                                                  result.ok = ThreeMFImport::Import(path, result.importedScene.get(), &importProgress);
                                              }
                                              else if (result.lower == "step" || result.lower == "stp")
                                              {
                                                  Solid* importedSolid = ImportSTEP(result.importedScene.get(), path);
                                                  result.ok = (importedSolid != nullptr);
                                              }
                                              else
                                              {
                                                  ReportImportProgress(&importProgress, "Unsupported import format.", 1.0f);
                                                  result.ok = false;
                                              }

                                              if (token.IsCancellationRequested())
                                              {
                                                  result.cancelled = true;
                                                  result.ok = false;
                                              }

                                              result.importerMs = std::chrono::duration<double, std::milli>(Clock::now() - tImporterStart).count();
                                              if (!result.ok)
                                                  result.importedScene.reset();
                                              // This runs on the worker thread; wake the blocked main loop so
                                              // ProcessDeferredImportIfAny() notices the ready result without
                                              // waiting for an unrelated input event.
                                              AppWakeEvent::Push();
                                              return result;
                                          });
}

void Display::DoFileImport()
{
    FileImport::OpenFileDialog(window, [this](const std::string &path)
                               {
                                   deferredImportPath = path;
                                   renderDirty = true;
                                   AppWakeEvent::Push();
                               });
}

void Display::RebuildFileTabs()
{
    // Compact tab style: natural layer-2 paragraph defaults (margin=INSET, padding=0) — matches Analysis children.

    const bool showPendingImportTab =
        !pendingImportTabStem.empty() && (importBusy || pendingImportTask.has_value());

    uiFiles->children.clear();
    uiFiles->children.reserve(openFiles.size() + 1 + (showPendingImportTab ? 1 : 0)); // tabs + optional import + "+"

    for (size_t i = 0; i < openFiles.size(); i++)
    {
        // Use "file_N" as the paragraph id — unique even if two files share the same name.
        // Visible label comes from line.text, not the id.
        Paragraph &tab = uiFiles->AddParagraph("file_" + std::to_string(i));
        tab.values.reserve(1);
        SectionLine &line = tab.values.emplace_back();
        line.text = std::filesystem::path(openFiles[i]).stem().string();
        line.selected = (!pendingImportTabActive && i == activeSceneIndex);
        line.onClick = [this, i]()
        {
            ClearPickHover();
            ClearCalibrateFacePicks();

            // Switching away from a Structure staging session is treated as Cancel, so the old tab
            // is restored to its pre-carve state before we move focus.
            CancelPendingStructureCarveJob();
            if (IsStructureStagingActive())
                RestoreStructureOriginalScene();

            Scene *selectedScene = ownedScenes[i].get();
            const bool sceneChanged = (scene != selectedScene || activeSceneIndex != i);
            scene = selectedScene;
            activeSceneIndex = i;
            pendingImportTabActive = false;
            if (sceneChanged)
            {
                UpdateScene();
                FrameScene();
                RefreshImportClosedVolumeContractFromScene();
            }
            // Live-carve continues on the newly focused tab if Structure stays active.
            if (activeTool == ActiveTool::Structure)
                BeginStructureStagingSession();
            SyncStructurePanelDerivedVisibility();
            SyncCalibrateImportPrerequisiteVisibility();
            pendingFileTabsRebuild = true;
            uiRenderer.MarkDirty();
        };
    }

    if (showPendingImportTab)
    {
        Paragraph &impTab = uiFiles->AddParagraph("file_pending_import");
        impTab.selected = pendingImportTabActive;
        impTab.onClick = [this]()
        {
            if (!pendingImportTabStem.empty() && (importBusy || pendingImportTask.has_value()))
            {
                pendingImportTabActive = true;
                pendingFileTabsRebuild = true;
                uiRenderer.MarkDirty();
                renderDirty = true;
            }
        };
        impTab.values.reserve(1);
        SectionLine &impLine = impTab.values.emplace_back();
        impLine.text.clear();
        impLine.bold = false;
        impLine.textDepth = 2;
        impLine.getMinContentWidthPx = [this]()
        {
            ImFont *f = uiRenderer.GetPixelImFont();
            if (!f)
                return 160.0f;
            const std::string preview = std::string("Importing ") + pendingImportTabStem;
            return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, preview.c_str()).x + ImGui::GetStyle().FramePadding.x * 4.0f;
        };
        impLine.imguiContent = [this](float w, float h, float /*iconOffset*/)
        {
            if (pendingImportTabStem.empty())
                return;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 o = ImGui::GetCursorScreenPos();
            const float pad = ImGui::GetStyle().FramePadding.x;
            const std::string phase =
                importProgressPhase.empty() ? std::string("Importing file...") : importProgressPhase;
            const std::string title = std::string("Importing ") + pendingImportTabStem + " - " +
                                      ImportProgressLabel(phase, importProgress01);
            glm::vec4 tc = Color::GetUIText(2);
            ImFont *font = uiRenderer.GetPixelImFont() ? uiRenderer.GetPixelImFont() : ImGui::GetFont();
            const float fs = font->FontSize;
            const float textX = o.x + pad;
            dl->AddText(font, fs, ImVec2(textX, o.y + pad * 0.5f),
                        ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a)), title.c_str());

            constexpr float barH = 3.0f;
            const float barY = o.y + h - barH - pad * 0.75f;
            const float bx0 = textX;
            const float titleW = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, title.c_str()).x;
            const float bx1 = std::min(textX + titleW, o.x + w - pad);
            if (bx1 <= bx0 + 2.0f)
                return;
            const float rr = barH * 0.5f;
            glm::vec4 trackCol = Color::GetUIText(1);
            trackCol.a *= 0.12f;
            dl->AddRectFilled(ImVec2(bx0, barY), ImVec2(bx1, barY + barH),
                              ImGui::GetColorU32(ImVec4(trackCol.r, trackCol.g, trackCol.b, trackCol.a)), rr);
            glm::vec4 fillCol = Color::GetAccent(2, 1.0f, 1.0f);
            float t = importProgress01;
            if (t < 0.0f)
                t = std::fmod(static_cast<float>(ImGui::GetTime()) * 0.55f, 1.0f);
            t = std::clamp(t, 0.0f, 1.0f);
            const float fillX = bx0 + (bx1 - bx0) * t;
            dl->AddRectFilled(ImVec2(bx0, barY), ImVec2(fillX, barY + barH),
                              ImGui::GetColorU32(ImVec4(fillCol.r, fillCol.g, fillCol.b, fillCol.a)), rr);
        };
    }

    // "+" import button — always at the end
    Paragraph &importTab = uiFiles->AddParagraph("+");
    importTab.values.reserve(1);
    SectionLine &importLine = importTab.values.emplace_back();
    importLine.iconDraw = Icons::ImportFile();
    importLine.onClick = [this]()
    { DoFileImport(); };

    uiRenderer.MarkDirty();
}

void Display::InitUI()
{
    float toolbarWidth = 2.0f;

    // ── Settings panel (left column: persistent app settings) ───────────────
    // Sections (Appearance, Viewport, Navigation) are populated further below.
    {
        RootPanel settingsDef;
        settingsDef.id = "Settings";
        settingsDef.bgParentDepth = 0;
        settingsDef.leftAnchor = PanelAnchor{nullptr, PanelAnchor::Left};
        settingsDef.topAnchor = PanelAnchor{nullptr, PanelAnchor::Top};
        settingsDef.bottomAnchor = PanelAnchor{nullptr, PanelAnchor::Bottom};
        settingsDef.header = Header{"Settings", 1.0f, 2};
        uiSettings = &uiRenderer.AddPanel(settingsDef);
        uiSettings->children.reserve(3); // Appearance, Viewport, Navigation
    }

    // ── Toolbar (column 2: tool selector) ────────────────────────────────────
    {
        RootPanel toolbarDef;
        toolbarDef.id = "Toolbar";
        toolbarDef.bgParentDepth = 0;
        toolbarDef.leftAnchor = PanelAnchor{uiSettings, PanelAnchor::Right};
        toolbarDef.topAnchor = PanelAnchor{nullptr, PanelAnchor::Top};
        toolbarDef.bottomAnchor = PanelAnchor{nullptr, PanelAnchor::Bottom};
        toolbarDef.width = toolbarWidth;
        uiToolbar = &uiRenderer.AddPanel(toolbarDef);
        uiToolbar->children.reserve(3);

        {
            Paragraph &p = uiToolbar->AddParagraph("ToolAnalysis");
            p.values.reserve(1);
            SectionLine &line = p.values.emplace_back();
            line.iconDraw = Icons::ToolAnalysis();
            line.fontScale = 1.4f;
            line.squareIconHit = true;
            line.selected = true; // Analysis is the default active tool
            line.onClick = [this]()
            {
                if (activeTool == ActiveTool::Analysis)
                {
                    uiAnalysis->visible = !uiAnalysis->visible;
                    if (!uiAnalysis->visible)
                    {
                        ClearPickHover();
                        ClearCalibrateFacePicks();
                    }
                    if (analysisEnabled != uiAnalysis->visible)
                    {
                        analysisEnabled = uiAnalysis->visible;
                        UpdateScene();
                    }
                    SyncToolbarToolVisualState();
                    uiRenderer.MarkDirty();
                    renderDirty = true;
                    return;
                }
                activeTool = ActiveTool::Analysis;
                pendingToolSwitch = true;
                renderDirty = true;
            };
            toolbarAnalysisLine = &line;
        }
        {
            Paragraph &p = uiToolbar->AddParagraph("ToolCalibrate");
            p.values.reserve(1);
            SectionLine &line = p.values.emplace_back();
            line.iconDraw = Icons::ToolCalibrate();
            line.fontScale = 1.4f;
            line.squareIconHit = true;
            line.onClick = [this]()
            {
                if (activeTool == ActiveTool::Calibrate)
                {
                    uiCalibrate->visible = !uiCalibrate->visible;
                    ClearPickHover();
                    ClearCalibrateFacePicks();
                    SyncToolbarToolVisualState();
                    uiRenderer.MarkDirty();
                    renderDirty = true;
                    return;
                }
                activeTool = ActiveTool::Calibrate;
                pendingToolSwitch = true;
                renderDirty = true;
            };
            toolbarCalibrateLine = &line;
        }
        {
            Paragraph &p = uiToolbar->AddParagraph("ToolStructure");
            p.values.reserve(1);
            SectionLine &line = p.values.emplace_back();
            line.iconDraw = Icons::ToolStructure();
            line.fontScale = 1.4f;
            line.squareIconHit = true;
            line.onClick = [this]()
            {
                if (activeTool == ActiveTool::Structure)
                {
                    uiStructure->visible = !uiStructure->visible;
                    ClearPickHover();
                    ClearCalibrateFacePicks();
                    ClearStructureFacePick();
                    RefreshStructurePreviewForRenderer();
                    MarkGeometryDirtyAll();
                    SyncToolbarToolVisualState();
                    uiRenderer.MarkDirty();
                    renderDirty = true;
                    return;
                }
                activeTool = ActiveTool::Structure;
                structureOptFaceExcludeStep = Icons::StepState::Active;
                SyncStructureOptionalPrereqRowStyle();
                pendingToolSwitch = true;
                renderDirty = true;
            };
            toolbarStructureLine = &line;
        }
    }

    // Files tab bar — spans from toolbar right edge to screen right
    RootPanel filesDef;
    filesDef.id = "Files";
    filesDef.bgParentDepth = 0;
    filesDef.horizontal = true;
    filesDef.leftAnchor = PanelAnchor{uiToolbar, PanelAnchor::Right};
    filesDef.rightAnchor = PanelAnchor{nullptr, PanelAnchor::Right};
    filesDef.topAnchor = PanelAnchor{nullptr, PanelAnchor::Top};
    filesDef.header = Header{"Files", 1.0f, 2};
    uiFiles = &uiRenderer.AddPanel(filesDef);
    RebuildFileTabs();

    // Analysis panel with sections
    RootPanel analysisDef;
    analysisDef.id = "Analysis";
    analysisDef.bgParentDepth = 0;
    analysisDef.leftAnchor = PanelAnchor{uiToolbar, PanelAnchor::Right};
    analysisDef.topAnchor = PanelAnchor{uiFiles, PanelAnchor::Bottom};
    uiAnalysis = &uiRenderer.AddPanel(analysisDef);

#if 1 // DEBUG: panel-only mode — sections/content hidden for layout debugging
    uiAnalysis->header = Header{"Analysis", 1.0f, 2};
    {
        Paragraph &sub = uiAnalysis->subtitle.emplace();
        sub.values.reserve(1);
        SectionLine &line = sub.values.emplace_back();
        line.text = "Detect possible 3D printing issues by analyzing geometry";
        line.textDepth = 1;
    }
    uiAnalysis->children.reserve(5); // stable pointers: Result + ImportAction + Verdict + Configs + Processing
    uiResult = &uiAnalysis->AddParagraph("Result");
    uiResult->visible = false;
    {
        PrerequisiteDef importDef;
        importDef.id          = "ImportAction";
        importDef.title       = "Import a file";
        importDef.leadingDraw = Icons::CheckBox(&analysisStepImport);
        importDef.active      = true;
        importDef.onClick     = [this]() { DoFileImport(); };
        uiAnalysis->AddParagraph(importDef.id) = BuildPrerequisiteParagraph(importDef);
        uiImportPara = &std::get<Paragraph>(uiAnalysis->children.back());
    }
    uiVerdict = &uiAnalysis->AddParagraph("Verdict");
    uiVerdict->visible = false;

    // Merged result+config rows — always present once a model is loaded.
    // Each row shows: [icon] [count label (flaw color)] · [param value (dim)] [unit]
    // DragFloat spans the full row; a left-zone InvisibleButton handles click-to-navigate.
    uiResult = &uiAnalysis->AddParagraph("Result");
    uiResult->visible = false;
    uiResult->values.reserve(4);

    // Helper: builds an imguiContent lambda for a merged flaw+param row.
    // flawResult    = member to read count/frameCallback from (captured by ref via this)
    // flawColor     = bright flaw color for count+label text
    // countLabel    = e.g. " overhang"  (leading space, singular; "s" appended when count>1)
    // paramLabel    = short label shown dim to the right of the value, e.g. "°" or " mm"
    // getValue      = getter lambda returning float
    // setValue      = setter lambda (float)
    // dragSpeed/min/max/fmt = DragFloat parameters

    auto makeFlawRow = [this](
                           SectionLine &line,
                           glm::vec4 flawColor,
                           Icons::DrawFn icon,
                           const char *countLabel, // singular, e.g. " overhang"
                           const char *plural,     // suffix when count>1, e.g. "s"
                           FlawResult Display::*flawMember,
                           float &param,
                           float dragSpeed, float dragMin, float dragMax,
                           const char *unit, // e.g. "°" or "mm"
                           const char *dragId,
                           const char *minWidthLabel, // e.g. "45°" for min width estimate
                           bool storageIsLengthMm, // true: param stored in mm; UI uses default unit + suffix parse
                           bool isAngle             // true = format "%.0f", false = "%.2f"/"%.1f" based on dragSpeed
                       )
    {
        auto lengthDisplay = storageIsLengthMm ? std::make_shared<float>(0.f) : nullptr;
        auto lengthText = storageIsLengthMm ? std::make_shared<std::array<char, 128>>() : nullptr;
        auto lenTextModeFlag = storageIsLengthMm ? std::make_shared<bool>(false) : nullptr;

        line.iconDraw = [this, flawMember, icon](ImDrawList *dl, float x, float midY, float s)
        {
            FlawResult &fr = this->*flawMember;
            if (fr.count == 0)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.35f);
            icon(dl, x, midY, s);
            if (fr.count == 0)
                ImGui::PopStyleVar();
        };

        // Minimum content width (excluding icon slot, which computeParagraphBox adds separately).
        line.getMinContentWidthPx = [this, countLabel, plural, minWidthLabel, unit, isAngle, storageIsLengthMm]() -> float
        {
            ImFont *f = uiRenderer.GetPixelImFont();
            if (!f)
                return 0.0f;
            float pad = ImGui::GetStyle().FramePadding.x;
            std::string longestCount = std::string("No") + countLabel + plural;
            float countW = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, longestCount.c_str()).x;
            std::string longestVal = isAngle ? std::string(minWidthLabel) + unit
                                             : (storageIsLengthMm ? std::string("0000.0000 in")
                                                                  : std::string(minWidthLabel) + " " + unit);
            float valW = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, longestVal.c_str()).x;
            constexpr float gap = 24.0f;
            return pad * 2.0f + countW + gap + valW;
        };

        line.imguiContent = [this, flawColor, countLabel, plural, flawMember,
                             &param, dragSpeed, dragMin, dragMax,
                             unit, dragId, isAngle, storageIsLengthMm, lengthDisplay, lengthText,
                             lenTextModeFlag](float w, float h, float iconOffset)
        {
            FlawResult &fr = this->*flawMember;
            const LengthUnit defaultLen = LengthUnitFromIndex(settings.defaultLengthUnit);
            glm::vec4 dimColor = Color::GetUIText(1);
            glm::vec4 dimLow = Color::GetUIText(0);

            UIStyle::PushInputStyle(h, dimColor);
            float normalPad = ImGui::GetStyle().FramePadding.x;
            ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
            float originX = rowOrigin.x;

            ImFont *rowFont = FontOrInteractiveRow(uiRenderer, nullptr);
            const float rowFs = rowFont ? rowFont->FontSize : ImGui::GetFontSize();

            // ── Left nav zone: InvisibleButton placed BEFORE DragFloat ──────────
            // Compute left zone width from the same string we draw (zero-count uses
            // "No…" + plural, which is wider than "0" + singular — mismatch used to
            // shrink the param zone and let the value hover tint overlap the title).
            char countBuf[64];
            if (fr.count > 0)
                snprintf(countBuf, sizeof(countBuf), "%zu%s%s", fr.count, countLabel,
                         fr.count > 1 ? plural : "");
            else
                snprintf(countBuf, sizeof(countBuf), "No%s%s", countLabel, plural);
            float leftW = iconOffset + (rowFont ? rowFont->CalcTextSizeA(rowFs, FLT_MAX, 0.0f, countBuf).x
                                                     : ImGui::CalcTextSize(countBuf).x) +
                           normalPad * 2.5f;
            leftW = std::min(leftW, w * 0.65f); // never crowd out the param zone

            bool navFired = false;
            bool showEdit = fr.editing || fr.focusPending;

            if (!showEdit)
            {
                char navId[64];
                snprintf(navId, sizeof(navId), "##nav%s", dragId);
                ImGui::InvisibleButton(navId, ImVec2(leftW, h));

                if (ImGui::IsItemActivated())
                {
                    fr.navTracking = true;
                    fr.navStart = ImGui::GetIO().MousePos;
                }
                if (fr.navTracking && !ImGui::IsItemActive())
                {
                    ImVec2 ep = ImGui::GetIO().MousePos;
                    float d = (ep.x - fr.navStart.x) * (ep.x - fr.navStart.x) +
                              (ep.y - fr.navStart.y) * (ep.y - fr.navStart.y);
                    if (d < 9.0f && fr.count > 0 && fr.frameCallback)
                        navFired = true;
                    fr.navTracking = false;
                }
                // Hover tint on left zone (only when there's something to navigate to)
                if (fr.count > 0 && fr.frameCallback)
                    UIStyle::DrawInputHoverTint(1);
                else if (ImGui::IsItemHovered())
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow); // no pointer when non-navigable
            }

            // Always position DragFloat at the right zone — even in edit mode.
            // Add a small gap in edit mode so the input field doesn't butt up against the label.
            float editGap = showEdit ? normalPad * 3.0f : 0.0f;

            // ── Right param zone: DragFloat ──────────────────────────────────────
            float rightW = w - leftW - editGap;
            const float paramLeft = originX + leftW + editGap;
            // Right edge of the value control (inside the same rounded frame as ImGui item).
            const float paramZoneRight = paramLeft + rightW;

            if (fr.requestEdit)
            {
                if (storageIsLengthMm && lenTextModeFlag && lengthText)
                {
                    *lenTextModeFlag = true;
                    std::snprintf(lengthText->data(), lengthText->size(), "%.6g %s",
                                  static_cast<double>(FromMillimeters(param, defaultLen)),
                                  LengthUnitAbbreviation(defaultLen));
                }
                ImGui::SetKeyboardFocusHere();
                fr.requestEdit = false;
                fr.focusPending = true;
                showEdit = true;
            }

            const char *fmt = showEdit ? (isAngle ? "%.0f" : (dragSpeed < 0.1f ? "%.2f" : "%.1f")) : "";
            if (storageIsLengthMm && !isAngle)
                fmt = showEdit ? "%.6g" : "";

            bool changed = false;
            bool committedEdit = false;

            if (storageIsLengthMm && lengthDisplay && lengthText && lenTextModeFlag &&
                *lenTextModeFlag)
            {
                ImGui::SetCursorScreenPos(ImVec2(paramZoneRight - rightW, rowOrigin.y));
                ImGui::SetNextItemWidth(rightW);
                char inputId[72];
                std::snprintf(inputId, sizeof(inputId), "##ltxt%s", dragId);
                bool textChanged = ImGui::InputText(inputId, lengthText->data(), lengthText->size(),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
                (void)textChanged;
                UIStyle::DrawInputHoverTint(1);
                fr.editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
                if (fr.editing)
                    fr.focusPending = false;
                committedEdit = *lenTextModeFlag && ImGui::IsItemDeactivated();
                if (committedEdit)
                {
                    float mm = param;
                    if (TryParseLengthToMm(std::string_view(lengthText->data()), defaultLen, mm))
                    {
                        param = mm;
                        *lengthDisplay = FromMillimeters(param, defaultLen);
                        changed = true;
                    }
                    else
                    {
                        std::snprintf(lengthText->data(), lengthText->size(), "%.6g %s",
                                      static_cast<double>(FromMillimeters(param, defaultLen)),
                                      LengthUnitAbbreviation(defaultLen));
                    }
                    *lenTextModeFlag = false;
                    fr.focusPending = false;
                }
            }
            else if (storageIsLengthMm && lengthDisplay)
            {
                float itemW = rightW;
                if (showEdit && !(lenTextModeFlag && *lenTextModeFlag))
                {
                    const float sw =
                        ImGui::CalcTextSize(LengthUnitAbbreviation(defaultLen)).x +
                        ImGui::GetStyle().ItemInnerSpacing.x * 2.0f;
                    itemW = std::max(rightW - sw, ImGui::GetFontSize() * 2.5f);
                }
                ImGui::SetCursorScreenPos(ImVec2(paramZoneRight - itemW, rowOrigin.y));
                ImGui::SetNextItemWidth(itemW);

                if (!ImGui::IsItemActive())
                    *lengthDisplay = FromMillimeters(param, defaultLen);
                const float mmPer = MillimetersPerUnit(defaultLen);
                const float duSpeed = (mmPer > 0.0f) ? dragSpeed / mmPer : dragSpeed;
                const float dmin = FromMillimeters(dragMin, defaultLen);
                const float dmax = FromMillimeters(dragMax, defaultLen);
                changed = ImGui::DragFloat(dragId, lengthDisplay.get(), duSpeed, dmin, dmax, fmt);
                committedEdit = ImGui::IsItemDeactivatedAfterEdit();
                UIStyle::DrawInputHoverTint(1);
                if (changed)
                    param = ToMillimeters(*lengthDisplay, defaultLen);
                if (committedEdit)
                    param = ToMillimeters(*lengthDisplay, defaultLen);
                if (ImGui::IsItemActivated())
                    *lenTextModeFlag = false;
                fr.editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
                if (fr.editing)
                    fr.focusPending = false;
                if (ImGui::IsItemActivated())
                {
                    fr.tracking = true;
                    fr.startPos = ImGui::GetIO().MousePos;
                }
                if (fr.tracking && !ImGui::IsItemActive())
                {
                    ImVec2 ep = ImGui::GetIO().MousePos;
                    float d = (ep.x - fr.startPos.x) * (ep.x - fr.startPos.x) +
                              (ep.y - fr.startPos.y) * (ep.y - fr.startPos.y);
                    if (d < 9.0f)
                        fr.requestEdit = true;
                    fr.tracking = false;
                }
            }
            else
            {
                ImGui::SetCursorScreenPos(ImVec2(paramLeft, rowOrigin.y));
                ImGui::SetNextItemWidth(rightW);
                changed = ImGui::DragFloat(dragId, &param, dragSpeed, dragMin, dragMax, fmt);
                committedEdit = ImGui::IsItemDeactivatedAfterEdit();
                UIStyle::DrawInputHoverTint(1);

                fr.editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
                if (fr.editing)
                    fr.focusPending = false;

                if (ImGui::IsItemActivated())
                {
                    fr.tracking = true;
                    fr.startPos = ImGui::GetIO().MousePos;
                }
                if (fr.tracking && !ImGui::IsItemActive())
                {
                    ImVec2 ep = ImGui::GetIO().MousePos;
                    float d = (ep.x - fr.startPos.x) * (ep.x - fr.startPos.x) +
                              (ep.y - fr.startPos.y) * (ep.y - fr.startPos.y);
                    if (d < 9.0f)
                        fr.requestEdit = true;
                    fr.tracking = false;
                }
            }

            // showEdit may change after InputText / DragFloat this frame
            showEdit = fr.editing || fr.focusPending;
            if (storageIsLengthMm && lenTextModeFlag && *lenTextModeFlag)
                showEdit = true;

            // ── Text overlay ─────────────────────────────────────────────────────
            float ty = ImGui::GetItemRectMin().y + ImGui::GetStyle().FramePadding.y;
            ImU32 flawCol = ImGui::GetColorU32(ImVec4(flawColor.r, flawColor.g, flawColor.b, flawColor.a));
            ImU32 dimCol = ImGui::GetColorU32(ImVec4(dimLow.r, dimLow.g, dimLow.b, dimLow.a));
            ImU32 dimColZero = ImGui::GetColorU32(ImVec4(dimLow.r, dimLow.g, dimLow.b, dimLow.a * 0.5f));

            // Left label — always visible (even during text edit); countBuf matches layout above.
            if (rowFont)
            {
                if (fr.count > 0)
                    ImGui::GetWindowDrawList()->AddText(rowFont, rowFs, ImVec2(originX + iconOffset + normalPad, ty),
                                                        flawCol, countBuf);
                else
                    ImGui::GetWindowDrawList()->AddText(rowFont, rowFs, ImVec2(originX + iconOffset + normalPad, ty),
                                                        dimColZero, countBuf);
            }
            else
            {
                if (fr.count > 0)
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(originX + iconOffset + normalPad, ty), flawCol, countBuf);
                else
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(originX + iconOffset + normalPad, ty), dimColZero, countBuf);
            }

            // Right side: readout and edit hints — align to param zone right edge (inside ImGui frame)
            const char *editUnitHint =
                (storageIsLengthMm && !isAngle) ? LengthUnitAbbreviation(defaultLen) : unit;
            const bool skipEditUnitOverlay =
                (storageIsLengthMm && !isAngle && lenTextModeFlag && *lenTextModeFlag);
            if (!showEdit)
            {
                char valBuf[48];
                if (isAngle)
                    snprintf(valBuf, sizeof(valBuf), "%.0f%s", param, unit);
                else if (storageIsLengthMm)
                    FormatLengthMmForDisplay(valBuf, sizeof(valBuf), param, defaultLen);
                else if (dragSpeed < 0.1f)
                    snprintf(valBuf, sizeof(valBuf), "%.2f %s", param, unit);
                else
                    snprintf(valBuf, sizeof(valBuf), "%.1f %s", param, unit);
                ImVec2 vs = rowFont ? rowFont->CalcTextSizeA(rowFs, FLT_MAX, 0.0f, valBuf) : ImGui::CalcTextSize(valBuf);
                ImU32 valCol = (fr.count > 0) ? dimCol : dimColZero;
                if (rowFont)
                    ImGui::GetWindowDrawList()->AddText(rowFont, rowFs,
                                                        ImVec2(paramZoneRight - normalPad - vs.x, ty), valCol, valBuf);
                else
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(paramZoneRight - normalPad - vs.x, ty), valCol, valBuf);
            }
            else if (!skipEditUnitOverlay)
            {
                ImVec2 us =
                    rowFont ? rowFont->CalcTextSizeA(rowFs, FLT_MAX, 0.0f, editUnitHint) : ImGui::CalcTextSize(editUnitHint);
                if (rowFont)
                    ImGui::GetWindowDrawList()->AddText(rowFont, rowFs,
                                                        ImVec2(paramZoneRight - normalPad - us.x, ty), dimCol, editUnitHint);
                else
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(paramZoneRight - normalPad - us.x, ty), dimCol, editUnitHint);
            }

            if (navFired)
                fr.frameCallback();
            if (changed || committedEdit)
            {
                auto &sl = SessionLogger::Instance();
                sl.state.overhangAngle = this->overhangAngle;
                sl.state.sharpCornerThreshold = this->sharpCornerThreshold;
                sl.state.instabilityMinWidth = this->instabilityMinWidth;
                sl.state.layerDifferenceMaxAreaDelta = this->layerDifferenceMaxAreaDelta;
                sl.state.layerHeight = this->layerHeight;
                sl.LogParamChange(std::string(dragId + 2), param);
                RebuildAnalysis();
                // Only trigger the analysis rerun on commit (mouse release or Enter). Calling
                // UpdateScene() on every drag frame launches a new async analysis task each frame,
                // which hides uiResult while the task runs and kills ImGui focus on the DragFloat.
                // MarkStyleDirty (not UpdateScene) is correct here: geometry hasn't changed, only
                // the analysis classification thresholds changed, so only a recolor is needed.
                if (committedEdit)
                    MarkStyleDirty();
            }
            UIStyle::PopInputStyle();
        };
    };

    glm::vec4 overhangColor = {Color::GetFace(FaceFlawKind::OVERHANG).r + 0.4f,
                               Color::GetFace(FaceFlawKind::OVERHANG).g + 0.2f,
                               Color::GetFace(FaceFlawKind::OVERHANG).b + 0.2f, 1.0f};
    glm::vec4 sharpCornerColor = {Color::GetFace(FaceFlawKind::SHARP_CORNER).r + 0.4f,
                                     Color::GetFace(FaceFlawKind::SHARP_CORNER).g + 0.4f,
                                     Color::GetFace(FaceFlawKind::SHARP_CORNER).b + 0.2f, 1.0f};
    glm::vec4 instabilityColor = {Color::GetFace(FaceFlawKind::INSTABILITY).r + 0.4f,
                                  Color::GetFace(FaceFlawKind::INSTABILITY).g + 0.3f,
                                  Color::GetFace(FaceFlawKind::INSTABILITY).b + 0.15f, 1.0f};
    glm::vec4 layerDifferenceColor = {Color::GetFace(FaceFlawKind::LAYER_DIFFERENCE).r + 0.3f,
                                      Color::GetFace(FaceFlawKind::LAYER_DIFFERENCE).g + 0.15f,
                                      Color::GetFace(FaceFlawKind::LAYER_DIFFERENCE).b + 0.4f, 1.0f};
    makeFlawRow(uiResult->values.emplace_back(), overhangColor,
                Icons::Overhang(overhangColor),
                " overhang", "s", &Display::flawOverhang,
                overhangAngle, 0.5f, 0.0f, 90.0f, "\u00b0", "##overhang", "90", false, true);

    makeFlawRow(uiResult->values.emplace_back(), sharpCornerColor,
                Icons::SharpCorner(sharpCornerColor),
                " sharp corner", "s", &Display::flawSharpCorner,
                sharpCornerThreshold, 1.0f, 0.0f, 180.0f, "°", "##sharpcorner", "54°", false, true);

    makeFlawRow(uiResult->values.emplace_back(), instabilityColor,
                Icons::Instability(instabilityColor),
                " instability", "s", &Display::flawInstability,
                instabilityMinWidth, 0.05f, 0.1f, 50.0f, "mm", "##instability", "2.0", true, false);

    makeFlawRow(uiResult->values.emplace_back(), layerDifferenceColor,
                Icons::LayerDifference(layerDifferenceColor),
                " layer difference", "s", &Display::flawLayerDifference,
                layerDifferenceMaxAreaDelta, 1.0f, 1.0f, 500.0f, "mm\u00b2", "##layerdifference", "50", false, false);

    uiAnalysisProcessing = &uiAnalysis->AddParagraph("Processing");
    uiAnalysisProcessing->visible = false;
    uiAnalysisProcessing->dimFill = true;
    uiAnalysisProcessing->padding = UIGrid::GAP * UIElement::INSET_RATIO * 0.85f;
    uiAnalysisProcessing->values.reserve(1);
    {
        SectionLine &line = uiAnalysisProcessing->values.emplace_back();
        line.text = "Analysing faces...";
        line.textDepth = 2;
    }

    RebuildAnalysis();

#endif // DEBUG: panel-only mode

    // Settings panel — left column; extends from bottom of Analysis to bottom of screen.
    // Covers appearance, viewport, and navigation configuration.
    settingsAccentHue = Color::GetAccentHue();
    settingsAccentSat = Color::GetAccentSat();
    ImFont *settingsBodyFont = uiRenderer.GetBodyImFont();

    // Helper: builds a DragFloat row with a left label and right value overlay.
    auto makeSettingsDrag = [this, settingsBodyFont](
                                SectionLine &line,
                                const char *label,
                                float &param,
                                float speed, float minVal, float maxVal,
                                const char *valueFmt, // complete snprintf format, e.g. "%.0f\u00b0"
                                const char *dragId,
                                std::function<void()> onChange)
    {
        line.getMinContentWidthPx = [this, settingsBodyFont, label]() -> float
        {
            ImFont *f = uiRenderer.GetPixelImFont();
            if (!f)
                f = settingsBodyFont;
            if (!f)
                return 0.0f;
            float pad = ImGui::GetStyle().FramePadding.x;
            float labelW = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, label).x;
            constexpr float minValueAreaW = 40.0f; // room for value text + drag affordance
            constexpr float gap = 24.0f;
            return pad * 2.0f + labelW + gap + minValueAreaW;
        };

        struct DragEditState
        {
            bool tracking = false;
            bool requestEdit = false;
            bool editing = false;
            bool focusPending = false;
            ImVec2 startPos{};
        };
        auto dragState = std::make_shared<DragEditState>();

        line.imguiContent = [this, label, &param, speed, minVal, maxVal,
                             valueFmt, dragId, onChange = std::move(onChange),
                             settingsBodyFont, dragState](float w, float h, float iconOffset)
        {
            glm::vec4 tcLabel = Color::GetUIText(2); // label: prominent
            glm::vec4 tcValue = Color::GetUIText(0); // value: subdued
            float pad = ImGui::GetStyle().FramePadding.x;

            UIStyle::PushInputStyle(h, tcLabel);
            ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
            float originX = rowOrigin.x;

            ImFont *rowFont = FontOrInteractiveRow(uiRenderer, settingsBodyFont);
            const float fs = rowFont ? rowFont->FontSize : ImGui::GetFontSize();

            bool showEdit = dragState->editing || dragState->focusPending;

            if (dragState->requestEdit)
            {
                ImGui::SetKeyboardFocusHere();
                dragState->requestEdit = false;
                dragState->focusPending = true;
                showEdit = true;
            }

            float dragW, dragOffsetX;
            if (showEdit)
            {
                // Edit mode: right zone only so ImGui cursor text doesn't overlap the label.
                float labelTextW =
                    rowFont ? rowFont->CalcTextSizeA(fs, FLT_MAX, 0.0f, label).x : ImGui::CalcTextSize(label).x;
                float leftW = std::min(iconOffset + pad + labelTextW + pad * 2.5f, w * 0.6f);
                dragOffsetX = leftW;
                dragW = w - leftW;
            }
            else
            {
                // Normal mode: full row width — label and value are painted on top.
                dragOffsetX = 0.0f;
                dragW = w;
            }

            ImGui::SetCursorScreenPos(ImVec2(originX + dragOffsetX, rowOrigin.y));
            ImGui::SetNextItemWidth(dragW);
            bool changed = ImGui::DragFloat(dragId, &param, speed, minVal, maxVal,
                                            showEdit ? valueFmt : "");
            UIStyle::DrawInputHoverTint(1);

            dragState->editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
            if (dragState->editing)
                dragState->focusPending = false;

            if (ImGui::IsItemActivated())
            {
                dragState->tracking = true;
                dragState->startPos = ImGui::GetIO().MousePos;
            }
            if (dragState->tracking && !ImGui::IsItemActive())
            {
                ImVec2 ep = ImGui::GetIO().MousePos;
                float dx = ep.x - dragState->startPos.x;
                float dy = ep.y - dragState->startPos.y;
                if (dx * dx + dy * dy < 9.0f)
                    dragState->requestEdit = true;
                dragState->tracking = false;
            }

            ImDrawList *dl = ImGui::GetWindowDrawList();
            float bottom = ImGui::GetItemRectMax().y - ImGui::GetStyle().FramePadding.y;

            // Label: always drawn over the drag widget.
            ImU32 labelCol = ImGui::GetColorU32(ImVec4(tcLabel.r, tcLabel.g, tcLabel.b, tcLabel.a));
            {
                float ty_label = bottom - fs;
                if (rowFont)
                    dl->AddText(rowFont, fs, ImVec2(originX + iconOffset + pad, ty_label), labelCol, label);
                else
                    dl->AddText(ImVec2(originX + iconOffset + pad, ty_label), labelCol, label);
            }

            // Value overlay: right edge, hidden during text edit (DragFloat renders it).
            if (!showEdit)
            {
                char valBuf[32];
                snprintf(valBuf, sizeof(valBuf), valueFmt, param);
                ImVec2 vs = rowFont ? rowFont->CalcTextSizeA(fs, FLT_MAX, 0.0f, valBuf) : ImGui::CalcTextSize(valBuf);
                float ty_value = bottom - fs;
                ImU32 valCol = ImGui::GetColorU32(ImVec4(tcValue.r, tcValue.g, tcValue.b, tcValue.a));
                if (rowFont)
                    dl->AddText(rowFont, fs, ImVec2(originX + w - pad - vs.x, ty_value), valCol, valBuf);
                else
                    dl->AddText(ImVec2(originX + w - pad - vs.x, ty_value), valCol, valBuf);
            }

            if (changed)
                onChange();
            UIStyle::PopInputStyle();
        };
    };

    // Same row layout as makeSettingsDrag, but `paramMm` is stored in millimeters; drag/parse uses
    // settings.defaultLengthUnit, with optional mm/cm/in/ft suffix when editing as text.
    auto makeSettingsLengthDrag = [this, settingsBodyFont](
                                      SectionLine &line,
                                      const char *label,
                                      float &paramMm,
                                      float speedMm, float minMm, float maxMm,
                                      const char *dragId,
                                      std::function<void()> onChange)
    {
        line.getMinContentWidthPx = [this, settingsBodyFont, label]() -> float
        {
            ImFont *f = uiRenderer.GetPixelImFont();
            if (!f)
                f = settingsBodyFont;
            if (!f)
                return 0.0f;
            float pad = ImGui::GetStyle().FramePadding.x;
            float labelW = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, label).x;
            constexpr float minValueAreaW = 56.0f;
            constexpr float gap = 24.0f;
            return pad * 2.0f + labelW + gap + minValueAreaW;
        };

        struct DragEditState
        {
            bool tracking = false;
            bool requestEdit = false;
            bool editing = false;
            bool focusPending = false;
            ImVec2 startPos{};
        };
        auto dragState = std::make_shared<DragEditState>();
        auto display = std::make_shared<float>(0.f);
        auto textBuf = std::make_shared<std::array<char, 128>>();
        auto lenText = std::make_shared<bool>(false);

        line.imguiContent = [this, label, &paramMm, speedMm, minMm, maxMm, dragId,
                             onChange = std::move(onChange), settingsBodyFont, dragState, display, textBuf,
                             lenText](float w, float h, float iconOffset)
        {
            const LengthUnit du = LengthUnitFromIndex(settings.defaultLengthUnit);
            glm::vec4 tcLabel = Color::GetUIText(2);
            glm::vec4 tcValue = Color::GetUIText(0);
            float pad = ImGui::GetStyle().FramePadding.x;

            UIStyle::PushInputStyle(h, tcLabel);
            ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
            float originX = rowOrigin.x;

            ImFont *rowFont = FontOrInteractiveRow(uiRenderer, settingsBodyFont);
            float labelFontSz = rowFont ? rowFont->FontSize : ImGui::GetFontSize();

            bool showEdit = dragState->editing || dragState->focusPending;

            if (dragState->requestEdit)
            {
                *lenText = true;
                std::snprintf(textBuf->data(), textBuf->size(), "%.6g %s",
                              static_cast<double>(FromMillimeters(paramMm, du)),
                              LengthUnitAbbreviation(du));
                ImGui::SetKeyboardFocusHere();
                dragState->requestEdit = false;
                dragState->focusPending = true;
                showEdit = true;
            }

            float dragW, dragOffsetX;
            if (showEdit)
            {
                float labelTextW =
                    rowFont ? rowFont->CalcTextSizeA(labelFontSz, FLT_MAX, 0.0f, label).x : ImGui::CalcTextSize(label).x;
                float leftW = std::min(iconOffset + pad + labelTextW + pad * 2.5f, w * 0.6f);
                dragOffsetX = leftW;
                dragW = w - leftW;
            }
            else
            {
                dragOffsetX = 0.0f;
                dragW = w;
            }

            const float paramZoneRight = originX + dragOffsetX + dragW;

            bool changed = false;
            if (*lenText)
            {
                ImGui::SetCursorScreenPos(ImVec2(paramZoneRight - dragW, rowOrigin.y));
                ImGui::SetNextItemWidth(dragW);
                char textId[80];
                std::snprintf(textId, sizeof(textId), "##slen%s", dragId);
                (void)ImGui::InputText(textId, textBuf->data(), textBuf->size(),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
                UIStyle::DrawInputHoverTint(1);
                dragState->editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
                if (dragState->editing)
                    dragState->focusPending = false;
                if (*lenText && ImGui::IsItemDeactivated())
                {
                    float mm = paramMm;
                    if (TryParseLengthToMm(std::string_view(textBuf->data()), du, mm))
                    {
                        paramMm = mm;
                        *display = FromMillimeters(paramMm, du);
                        changed = true;
                    }
                    else
                    {
                        std::snprintf(textBuf->data(), textBuf->size(), "%.6g %s",
                                      static_cast<double>(FromMillimeters(paramMm, du)),
                                      LengthUnitAbbreviation(du));
                    }
                    *lenText = false;
                    dragState->focusPending = false;
                }
            }
            else
            {
                float itemW = dragW;
                if (showEdit && !*lenText)
                {
                    const float sw =
                        ImGui::CalcTextSize(LengthUnitAbbreviation(du)).x +
                        ImGui::GetStyle().ItemInnerSpacing.x * 2.0f;
                    itemW = std::max(dragW - sw, ImGui::GetFontSize() * 2.5f);
                }
                ImGui::SetCursorScreenPos(ImVec2(paramZoneRight - itemW, rowOrigin.y));
                ImGui::SetNextItemWidth(itemW);

                if (!ImGui::IsItemActive())
                    *display = FromMillimeters(paramMm, du);
                const float mmPer = MillimetersPerUnit(du);
                const float duSpeed = (mmPer > 0.0f) ? speedMm / mmPer : speedMm;
                const float dmin = FromMillimeters(minMm, du);
                const float dmax = FromMillimeters(maxMm, du);
                changed = ImGui::DragFloat(dragId, display.get(), duSpeed, dmin, dmax,
                                           showEdit ? "%.6g" : "");
                if (changed)
                    paramMm = ToMillimeters(*display, du);
                if (ImGui::IsItemDeactivatedAfterEdit())
                    paramMm = ToMillimeters(*display, du);
                UIStyle::DrawInputHoverTint(1);
                if (ImGui::IsItemActivated())
                    *lenText = false;

                dragState->editing = ImGui::IsItemActive() && ImGui::GetIO().WantTextInput;
                if (dragState->editing)
                    dragState->focusPending = false;

                if (ImGui::IsItemActivated())
                {
                    dragState->tracking = true;
                    dragState->startPos = ImGui::GetIO().MousePos;
                }
                if (dragState->tracking && !ImGui::IsItemActive())
                {
                    ImVec2 ep = ImGui::GetIO().MousePos;
                    float dx = ep.x - dragState->startPos.x;
                    float dy = ep.y - dragState->startPos.y;
                    if (dx * dx + dy * dy < 9.0f)
                        dragState->requestEdit = true;
                    dragState->tracking = false;
                }
            }

            showEdit = dragState->editing || dragState->focusPending;
            if (*lenText)
                showEdit = true;

            ImDrawList *dl = ImGui::GetWindowDrawList();
            float bottom = ImGui::GetItemRectMax().y - ImGui::GetStyle().FramePadding.y;

            ImU32 labelCol = ImGui::GetColorU32(ImVec4(tcLabel.r, tcLabel.g, tcLabel.b, tcLabel.a));
            {
                float ty_label = bottom - labelFontSz;
                if (rowFont)
                    dl->AddText(rowFont, labelFontSz, ImVec2(originX + iconOffset + pad, ty_label), labelCol, label);
                else
                    dl->AddText(ImVec2(originX + iconOffset + pad, ty_label), labelCol, label);
            }

            if (!showEdit)
            {
                char valBuf[48];
                FormatLengthMmForDisplay(valBuf, sizeof(valBuf), paramMm, du);
                ImVec2 vs =
                    rowFont ? rowFont->CalcTextSizeA(labelFontSz, FLT_MAX, 0.0f, valBuf) : ImGui::CalcTextSize(valBuf);
                float ty_value = bottom - labelFontSz;
                ImU32 valCol = ImGui::GetColorU32(ImVec4(tcValue.r, tcValue.g, tcValue.b, tcValue.a));
                if (rowFont)
                    dl->AddText(rowFont, labelFontSz, ImVec2(paramZoneRight - pad - vs.x, ty_value), valCol, valBuf);
                else
                    dl->AddText(ImVec2(paramZoneRight - pad - vs.x, ty_value), valCol, valBuf);
            }
            else if (!*lenText)
            {
                const char *abbr = LengthUnitAbbreviation(du);
                ImVec2 us =
                    rowFont ? rowFont->CalcTextSizeA(labelFontSz, FLT_MAX, 0.0f, abbr) : ImGui::CalcTextSize(abbr);
                float ty_value = bottom - labelFontSz;
                ImU32 dimU = ImGui::GetColorU32(ImVec4(tcValue.r, tcValue.g, tcValue.b, tcValue.a));
                if (rowFont)
                    dl->AddText(rowFont, labelFontSz, ImVec2(paramZoneRight - pad - us.x, ty_value), dimU, abbr);
                else
                    dl->AddText(ImVec2(paramZoneRight - pad - us.x, ty_value), dimU, abbr);
            }

            if (changed)
                onChange();
            UIStyle::PopInputStyle();
        };
    };

    // Settings panel was created early in InitUI (left edge; toolbar sits to its right).
    // Add sections to the existing uiSettings panel here.

    // ── Appearance ────────────────────────────────────────────────────────────
    Section &appearanceSection = uiSettings->AddSection("Appearance");
    appearanceSection.header = Header{"Appearance", 1.0f, 2};
    appearanceSection.tightHeader = true;
    appearanceSection.children.reserve(1);

    // All appearance rows in one paragraph — no splitters between them.
    Paragraph &appearancePara = appearanceSection.AddParagraph("AppearanceRows");
    // Theme + Accent + Contrast are appended below; reserve all so stored select pointers remain valid.
    appearancePara.values.reserve(3);

    // Theme selector: System / Light / Dark pill
    {
        SectionLine &themeSelect = appearancePara.values.emplace_back();
        themeSelect.text = "Theme";
        Select sel;
        sel.options = {
            {"System", Icons::ThemeSystem()},
            {"Light", Icons::ThemeLight()},
            {"Dark", Icons::ThemeDark()},
        };
        sel.activeIndex = static_cast<int>(themeMode);
        sel.onChange = [this](int i)
        {
            themeMode = static_cast<ThemeMode>(i);
            ApplyTheme();
            uiRenderer.MarkDirty();
        };
        themeSelect.select = std::move(sel);
        uiAppearanceThemeSelect = &themeSelect.select.value();
    }

    // Accent selector: System / Color pill — native select, identical layout to Theme.
    // onActiveClick on zone 1 (Color) opens the HSV picker popup; postDraw hosts it in the same window.
    {
        SectionLine &accentSel = appearancePara.values.emplace_back();
        accentSel.text = "Accent";
        Select sel;
        sel.options = {
            {"System", Icons::ThemeSystem()},
            {"Custom", Icons::AccentCustom()},
        };
        sel.activeIndex = settingsAccentUseSystem ? 0 : 1;
        sel.onChange = [this](int i)
        {
            settingsAccentUseSystem = (i == 0);
            if (settingsAccentUseSystem)
            {
                float hue, sat;
                if (SystemAccent::GetHueSat(hue, sat))
                {
                    // Keep saved custom hue/sat for when user switches back to Custom — only drive live color from OS.
                    Color::SetAccent(hue, sat);
                    uiRenderer.MarkDirty();
                    if (scene && (!scene->solids.empty() || !scene->faces.empty()))
                        MarkStyleDirty();
                    MarkPickDirty();
                }
            }
            else
            {
                Color::SetAccent(settingsAccentHue, settingsAccentSat);
                uiRenderer.MarkDirty();
                if (scene && (!scene->solids.empty() || !scene->faces.empty()))
                    MarkStyleDirty();
                MarkPickDirty();
            }
        };
        sel.onActiveClick = [this]()
        {
            if (!settingsAccentUseSystem) // only Custom zone can be active here
                settingsOpenAccentPicker = true;
        };
        sel.postDraw = [this]()
        {
            if (settingsOpenAccentPicker)
            {
                ImGui::OpenPopup("##accentPicker");
                settingsOpenAccentPicker = false;
            }
            if (ImGui::BeginPopup("##accentPicker"))
            {
                float hsv[3] = {settingsAccentHue / 360.0f, settingsAccentSat, 1.0f};
                float col4[4] = {};
                ImGui::ColorConvertHSVtoRGB(hsv[0], hsv[1], hsv[2], col4[0], col4[1], col4[2]);
                col4[3] = 1.0f;
                if (ImGui::ColorPicker4("##picker", col4,
                                        ImGuiColorEditFlags_NoAlpha |
                                            ImGuiColorEditFlags_DisplayHSV |
                                            ImGuiColorEditFlags_InputRGB))
                {
                    float h2, s2, v2;
                    ImGui::ColorConvertRGBtoHSV(col4[0], col4[1], col4[2], h2, s2, v2);
                    settingsAccentHue = h2 * 360.0f;
                    settingsAccentSat = s2;
                    Color::SetAccent(settingsAccentHue, settingsAccentSat);
                    uiRenderer.MarkDirty();
                    if (scene && (!scene->solids.empty() || !scene->faces.empty()))
                        MarkStyleDirty();
                    MarkPickDirty();
                }
                ImGui::EndPopup();
            }
        };
        accentSel.select = std::move(sel);
        uiAppearanceAccentSelect = &accentSel.select.value();
    }

    makeSettingsDrag(appearancePara.values.emplace_back(), "Contrast", UserTuning::contrast,
                     0.01f, 0.0f, 1.0f, "%.2f", "##contrast",
                     [this]()
                     {
                         UserTuning::DeriveFromContrast();
                         Color::SetUiDepthStep(UserTuning::uiDepthStep);
                         viewportRenderer.RegenerateGrid();
                         uiRenderer.MarkDirty();
                         if (scene && (!scene->solids.empty() || !scene->faces.empty()))
                            MarkStyleDirty();
                        MarkPickDirty();
                     });

    // ── Viewport ──────────────────────────────────────────────────────────────
    Section &viewportSection = uiSettings->AddSection("Viewport");
    viewportSection.header = Header{"Viewport", 1.0f, 2};
    viewportSection.tightHeader = true;
    viewportSection.children.reserve(1);

    Paragraph &gridPara = viewportSection.AddParagraph("GridSize");
    gridPara.values.reserve(3);

    {
        SectionLine &unitSel = gridPara.values.emplace_back();
        unitSel.text = "Length unit";
        Select sel;
        sel.textOnly = true;
        sel.options = {
            {"mm", {}},
            {"cm", {}},
            {"in", {}},
            {"ft", {}},
        };
        sel.activeIndex = std::clamp(settings.defaultLengthUnit, 0, 3);
        sel.onChange = [this](int i)
        {
            settings.defaultLengthUnit = std::clamp(i, 0, 3);
            SyncGridLayoutFromSettings();
            SaveSettings();
            uiRenderer.MarkDirty();
            renderDirty = true;
        };
        unitSel.select = std::move(sel);
        uiDefaultLengthUnitSelect = &unitSel.select.value();
    }

    makeSettingsDrag(gridPara.values.emplace_back(), "Grid size", settings.gridCellsAlongAxis,
                     1.0f, 4.0f, 8192.0f, "%.0f", "##gridCellsAlong",
                     [this]()
                     {
                         SyncGridLayoutFromSettings();
                         SaveSettings();
                     });

    makeSettingsDrag(gridPara.values.emplace_back(), "Low angle fade",
                     settings.gridPlaneTiltMinOpacity,
                     0.01f, 0.0f, 1.0f, "%.2f", "##gridPlaneTiltMin",
                     [this]()
                     {
                         settings.gridPlaneTiltMinOpacity =
                             std::clamp(settings.gridPlaneTiltMinOpacity, 0.0f, 1.0f);
                         viewportRenderer.SetGridPlaneTiltMinOpacity(settings.gridPlaneTiltMinOpacity);
                         SaveSettings();
                         renderDirty = true;
                     });

    // ── Navigation ───────────────────────────────────────────────────────────────────────────
    Section &navSection = uiSettings->AddSection("Navigation");
    navSection.header = Header{"Navigation", 1.0f, 2};
    navSection.tightHeader = true;
    navSection.children.reserve(1);

    Paragraph &sensPara = navSection.AddParagraph("MouseSens");
    sensPara.values.reserve(1);
    makeSettingsDrag(sensPara.values.emplace_back(), "Sensitivity", mouseSensitivity,
                     1.0f, 1.0f, 500.0f, "%.0f", "##mouseSens",
                     [this]()
                     { renderDirty = true; });
    makeSettingsDrag(sensPara.values.emplace_back(), "Snap", UserTuning::snap,
                     0.01f, 0.0f, 1.0f, "%.2f", "##snapMaster",
                     [this]()
                     {
                         UserTuning::DeriveFromSnap();
                         renderDirty = true;
                     });

    // ── Calibrate panel ───────────────────────────────────────────────────────
    {
        ToolPanelDef calibDef;
        calibDef.id = "Calibrate";
        calibDef.name = "Calibrate";
        calibDef.description = "Calibrate 3D printer accuracy by making measurements.";
        calibDef.flattenParameters = false;
        calibDef.showSectionHeaders = true;
        calibDef.sectionHeadersCollapsible = false;
        calibDef.parametersSectionTitle = "Parameters";
        calibDef.hasCalculator = true;
        calibDef.maxCalculatorLines = 1;
        calibDef.calculatorSectionTitle = "Result";

        // ── Prerequisites ──────────────────────────────────────────────────
        calibDef.prerequisites.reserve(4);
        calibDef.prerequisites.push_back({"CalibImport", "Import a file", "",
                                          Icons::CheckBox(&calibStepImport), false, true,
                                          [this]()
                                          { DoFileImport(); }});
        calibDef.prerequisites.push_back(
            {"CalibImportClosed", "Geometry clean (tools)",
             "Required for Structure and calibration picks on faces.",
             Icons::CheckBox(&calibStepImportClosedVolume), false, true, {}});
        calibDef.prerequisites.push_back({"CalibPoint1", "Plot measurement point", "to measure against",
                                          Icons::CheckBox(&calibStepPoint1), false, false});
        calibDef.prerequisites.push_back({"CalibPoint2", "Plot measurement point", "parallel point to first selection",
                                          Icons::CheckBox(&calibStepPoint2), false, false});

        // ── Parameters — print measurement InputFloat ──────────────────────
        // TODO(Calibrate): Inline CAD nominal span (and pick/span status when needed) on this row so
        // CalibDerived can stay compensation-only; see CalibDerived for messages still on the row below.
        {
            ParameterDef pm;
            pm.id = "CalibMeasure";
            pm.line.iconDraw = Icons::StepDot(&calibStepMeasure);
            pm.line.getMinContentWidthPx = [this, settingsBodyFont]() -> float
            {
                ImFont *f = uiRenderer.GetPixelImFont();
                if (!f)
                    f = settingsBodyFont;
                if (!f)
                    return 0.0f;
                float pad = ImGui::GetStyle().FramePadding.x;
                float labelW = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, "Print measurement").x;
                return pad * 2.0f + labelW + 24.0f + 48.0f;
            };
            auto pmEditing = std::make_shared<bool>(false);
            auto calibFocusRequest = std::make_shared<bool>(false);
            auto calibEditHadFocus = std::make_shared<bool>(false);
            auto calibText = std::make_shared<std::array<char, 128>>();
            calibText->data()[0] = '\0';
            pm.line.imguiContent = [this, settingsBodyFont, pmEditing, calibText, calibFocusRequest, calibEditHadFocus](float w, float h, float iconOffset)
            {
                (void)iconOffset;
                const LengthUnit du = LengthUnitFromIndex(settings.defaultLengthUnit);
                glm::vec4 tcLabel = Color::GetUIText(2);
                glm::vec4 tcValue = Color::GetUIText(0);
                float pad = ImGui::GetStyle().FramePadding.x;

                UIStyle::PushInputStyle(h, tcLabel);
                ImVec2 rowOrigin = ImGui::GetCursorScreenPos();

                ImFont *rowFont = FontOrInteractiveRow(uiRenderer, settingsBodyFont);
                const float fs = rowFont ? rowFont->FontSize : ImGui::GetFontSize();
                float labelTextW =
                    rowFont ? rowFont->CalcTextSizeA(fs, FLT_MAX, 0.0f, "Print measurement").x : ImGui::CalcTextSize("Print measurement").x;
                // Keep the input visually closer to the label (was too far right).
                float leftW = pad + labelTextW + pad * 1.25f;
                float inputW = w - leftW;

                bool changed = false;

                // Idle = hit target + drawn readout; edit = real InputText only. A ReadOnly InputText
                // with hidden text was crashing on focus loss in this nested ToolPanel window.
                if (*pmEditing)
                {
                    if (*calibFocusRequest)
                    {
                        ImGui::SetKeyboardFocusHere();
                        *calibFocusRequest = false;
                    }
                    ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x + leftW, rowOrigin.y));
                    ImGui::SetNextItemWidth(inputW);
                    const bool enterCommit = ImGui::InputText("##calibMeasured", calibText->data(), calibText->size(),
                                                              ImGuiInputTextFlags_EnterReturnsTrue);
                    UIStyle::DrawInputHoverTint(1);

                    if (ImGui::IsItemActivated() || ImGui::IsItemActive())
                        *calibEditHadFocus = true;
                    // Replacing InvisibleButton with InputText can yield IsItemDeactivated on the same
                    // transition before the field ever becomes active — that was closing edit immediately.
                    const bool leaveEdit = enterCommit || (*calibEditHadFocus && ImGui::IsItemDeactivated());
                    if (*pmEditing && leaveEdit)
                    {
                        float mm = calibMeasured;
                        if (TryParseLengthToMm(std::string_view(calibText->data()), du, mm))
                        {
                            calibMeasured = mm;
                            changed = true;
                        }
                        else
                        {
                            std::snprintf(calibText->data(), calibText->size(), "%.6g %s",
                                          static_cast<double>(FromMillimeters(calibMeasured, du)),
                                          LengthUnitAbbreviation(du));
                        }
                        *pmEditing = false;
                        *calibEditHadFocus = false;
                    }
                }
                else
                {
                    std::snprintf(calibText->data(), calibText->size(), "%.6g %s",
                                  static_cast<double>(FromMillimeters(calibMeasured, du)),
                                  LengthUnitAbbreviation(du));
                    ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x, rowOrigin.y));
                    const float hitH = std::max(h, ImGui::GetFrameHeight());
                    ImGui::InvisibleButton("##calibMeasured", ImVec2(w, hitH));
                    UIStyle::DrawInputHoverTint(1);
                    if (ImGui::IsItemClicked())
                    {
                        *pmEditing = true;
                        *calibFocusRequest = true;
                        *calibEditHadFocus = false;
                    }
                }

                ImDrawList *dl = ImGui::GetWindowDrawList();
                float itemBottom = ImGui::GetItemRectMax().y;
                const float cellRight = ImGui::GetItemRectMax().x;
                float labelTextH =
                    rowFont ? rowFont->CalcTextSizeA(fs, FLT_MAX, 0.0f, "Print measurement").y : ImGui::CalcTextSize("Print measurement").y;
                const bool errOnPrint = calibToolError.has_value() &&
                                        calibToolError->relatedParameterLabel == kCalibPrintMeasurementLabel;
                glm::vec4 labelColRgb = errOnPrint ? Color::GetAccent(2, 1.0f, 1.2f) : tcLabel;
                ImU32 labelCol = ImGui::GetColorU32(ImVec4(labelColRgb.r, labelColRgb.g, labelColRgb.b, labelColRgb.a));
                if (rowFont)
                    dl->AddText(rowFont, fs, ImVec2(rowOrigin.x + pad, itemBottom - labelTextH), labelCol, "Print measurement");
                else
                    dl->AddText(ImVec2(rowOrigin.x + pad, itemBottom - labelTextH), labelCol, "Print measurement");

                // Readout / edit: keep inside the input frame; full string includes unit when idle.
                if (!*pmEditing)
                {
                    char valueBuf[48];
                    FormatLengthMmForDisplay(valueBuf, sizeof(valueBuf), calibMeasured, du);
                    ImFont *valFont = rowFont;
                    if (!valFont && ImGui::GetIO().Fonts && ImGui::GetIO().Fonts->Fonts.Size > 0)
                        valFont = ImGui::GetIO().Fonts->Fonts[0];
                    if (valFont)
                    {
                        ImVec2 vs = valFont->CalcTextSizeA(valFont->FontSize, FLT_MAX, 0.0f, valueBuf);
                        ImU32 unitCol = ImGui::GetColorU32(ImVec4(tcValue.r, tcValue.g, tcValue.b, tcValue.a));
                        dl->AddText(valFont, valFont->FontSize, ImVec2(cellRight - pad - vs.x, itemBottom - vs.y), unitCol, valueBuf);
                    }
                }

                if (changed)
                {
                    RefreshCalibCompensation();
                    uiRenderer.MarkDirty();
                    renderDirty = true;
                }
                UIStyle::PopInputStyle();
            };
            calibDef.parameters.push_back(std::move(pm));
        }

        ParameterDef pmDer;
            pmDer.id = "CalibDerived";
            pmDer.line.iconDraw = Icons::StepDot(&calibStepMeasure);
            pmDer.line.getMinContentWidthPx = [this, settingsBodyFont]() -> float
            {
                ImFont *f = uiRenderer.GetPixelImFont();
                if (!f)
                    f = settingsBodyFont;
                if (!f)
                    return 0.0f;
                float pad = ImGui::GetStyle().FramePadding.x;
                float labelW = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f,
                                                "First-layer excess (printed \xe2\x88\x92 CAD)").x;
                return pad * 2.0f + labelW + 72.0f;
            };
            pmDer.line.imguiContent = [this, settingsBodyFont](float w, float h, float iconOffset)
            {
                (void)h;
                (void)iconOffset;
                glm::vec4 tcLabel = Color::GetUIText(2);
                glm::vec4 tcValue = Color::GetUIText(0);
                float pad = ImGui::GetStyle().FramePadding.x;
                ImVec2 row0 = ImGui::GetCursorScreenPos();
                ImDrawList *dl = ImGui::GetWindowDrawList();

                ImFont *rowFont = FontOrInteractiveRow(uiRenderer, settingsBodyFont);
                const float fs = rowFont ? rowFont->FontSize : ImGui::GetFontSize();
                float lh = rowFont ? rowFont->CalcTextSizeA(fs, FLT_MAX, 0.0f, "Mg").y : ImGui::GetTextLineHeight();
                const float rowHitH = std::max(lh * 1.35f, ImGui::GetFrameHeight());
                const float y0 = row0.y + pad * 0.25f;

                const bool missingFaces =
                    !CalibSlotHasPick(calibFacePoint1, calibEdgePoint1) ||
                    !CalibSlotHasPick(calibFacePoint2, calibEdgePoint2);
                const bool spanBad = !missingFaces && calibNominal <= 1e-5f;
                const bool importDone = ImportAllowsGeometryDependentTools();
                const bool firstFaceDone = calibStepPoint1 == Icons::StepState::Done;

                if (missingFaces)
                {
                    // The derived row should not reserve result space until a full span exists.
                    if (!importDone || !firstFaceDone)
                    {
                        ImGui::Dummy(ImVec2(w, pad));
                        return;
                    }
                    ImGui::Dummy(ImVec2(w, pad));
                    return;
                }
                if (calibToolError.has_value())
                {
                    const ToolUserErrorPayload &te = *calibToolError;
                    const float errH = DrawToolUserErrorCopyBlock(row0.x, y0, w, pad, rowFont, te.code, te.message,
                                                                  te.relatedParameterLabel, "calibErr");
                    ImGui::Dummy(ImVec2(w, errH + pad));
                    return;
                }
                if (spanBad)
                {
                    CalibDrawCopyableResultRow(dl, row0.x, y0, w, rowHitH, pad, nullptr, tcLabel, tcValue,
                                               "Could not estimate span (try parallel faces).", "", "spanBad");
                    ImGui::Dummy(ImVec2(w, rowHitH + pad));
                    return;
                }

                float y = y0;
                if (calibCompensationValid && calibWorkflow != CalibWorkflow::None)
                {
                    char valB[48] = {};
                    const char *lab = "";
                    switch (calibWorkflow)
                    {
                    case CalibWorkflow::Contour:
                        lab = "Shrinkage";
                        std::snprintf(valB, sizeof(valB), "%.4f", calibContourScale);
                        CalibDrawCopyableResultRow(dl, row0.x, y, w, rowHitH, pad, nullptr, tcLabel, tcValue,
                                                   lab, valB, "contour");
                        break;
                    case CalibWorkflow::Hole:
                        lab = "Hole Radius Offset";
                        std::snprintf(valB, sizeof(valB), "%.3f mm", calibHoleOffsetMm);
                        CalibDrawCopyableResultRow(dl, row0.x, y, w, rowHitH, pad, nullptr, tcLabel, tcValue,
                                                   lab, valB, "hole");
                        break;
                    case CalibWorkflow::ElephantFoot:
                        lab = "First-layer excess (printed \xe2\x88\x92 CAD)";
                        std::snprintf(valB, sizeof(valB), "%.3f mm", calibElephantFootMm);
                        CalibDrawCopyableResultRow(dl, row0.x, y, w, rowHitH, pad, nullptr, tcLabel, tcValue,
                                                   lab, valB, "elephant");
                        break;
                    default:
                        break;
                    }
                }
                else
                {
                    CalibDrawCopyableResultRow(dl, row0.x, y, w, rowHitH, pad, nullptr, tcLabel, tcValue,
                                               "Adjust print measurement to compute compensation.", "", "hint");
                }

                ImGui::Dummy(ImVec2(w, rowHitH + pad));
            };
        // `pmDer` is moved into the Calculator ("Result") section after `BuildToolPanel`.

        RootPanel calibPanel = BuildToolPanel(calibDef);
        calibPanel.visible = false;
        calibPanel.leftAnchor = PanelAnchor{uiToolbar, PanelAnchor::Right};
        calibPanel.topAnchor = PanelAnchor{uiFiles, PanelAnchor::Bottom};
        uiCalibrate = &uiRenderer.AddPanel(calibPanel);
        // Root `AddParagraph` must not run after we take `Paragraph*` into nested Sections — a realloc of
        // `uiCalibrate->children` moves every `Section`/`Paragraph` by value and invalidates those
        // pointers (same failure mode as Structure HoverHint below). Reserve headroom and append the
        // extra root paragraphs **before** `FindSection` bindings.
        uiCalibrate->children.reserve(uiCalibrate->children.size() + 4);

        uiCalibrateProcessing = &uiCalibrate->AddParagraph("Processing");
        uiCalibrateProcessing->visible = false;
        uiCalibrateProcessing->dimFill = true;
        uiCalibrateProcessing->padding = UIGrid::GAP * UIElement::INSET_RATIO * 0.85f;
        uiCalibrateProcessing->values.reserve(1);
        {
            SectionLine &line = uiCalibrateProcessing->values.emplace_back();
            line.text = "Refreshing calibration...";
            line.textDepth = 2;
        }

        calibPara_OpenBoundaryBanner = &uiCalibrate->AddParagraph("CalibOpenBoundaryBanner");
        calibPara_OpenBoundaryBanner->visible = false;
        calibPara_OpenBoundaryBanner->padding = UIGrid::GAP * UIElement::INSET_RATIO * 0.85f;
        calibPara_OpenBoundaryBanner->values.reserve(1);
        {
            SectionLine &el = calibPara_OpenBoundaryBanner->values.emplace_back();
            el.imguiContent = [this, settingsBodyFont](float w, float h, float)
            {
                (void)h;
                if (!importOpenBoundaryToolPayload.has_value())
                {
                    ImGui::Dummy(ImVec2(w, 1.f));
                    return;
                }
                ImFont *rowFont = FontOrInteractiveRow(uiRenderer, settingsBodyFont);
                const float pad = ImGui::GetStyle().FramePadding.x;
                const ImVec2 row0 = ImGui::GetCursorScreenPos();
                const ToolUserErrorPayload &te = *importOpenBoundaryToolPayload;
                const float errH = DrawToolUserErrorCopyBlock(row0.x, row0.y, w, pad, rowFont, te.code, te.message,
                                                              te.relatedParameterLabel, "oobImp");
                ImGui::SetCursorScreenPos(ImVec2(row0.x, row0.y + errH + pad));
                if (ImGui::Button("Fix##oobFix"))
                {
                    bool fixed = false;
                    const std::string &code = importOpenBoundaryToolPayload->code;
                    if (code == "IMPORT_GEOM_OPENBOUNDARY_SELF_INTERSECTION")
                    {
                        const bool splitFixed = TryFixSelfIntersectionForActiveScene();
                        // Avoid destructive fallback: if split fails, do not auto-run open-boundary
                        // repair in the same click. Keep failure non-mutating for retry/debugging.
                        if (splitFixed)
                        {
                            // Re-check geometry state after split; open-boundary repair may still be needed.
                            (void)TryFixOpenBoundaryForActiveScene();
                            fixed = true;
                        }
                    }
                    else if (code == "IMPORT_SELF_INTERSECTION")
                        fixed = TryFixSelfIntersectionForActiveScene();
                    else
                        fixed = TryFixOpenBoundaryForActiveScene();
                    if (!fixed)
                    {
                        importOpenBoundaryToolPayload.emplace(ToolUserErrorPayload{
                            (code == "IMPORT_SELF_INTERSECTION" ||
                             code == "IMPORT_GEOM_OPENBOUNDARY_SELF_INTERSECTION")
                                ? std::string("IMPORT_SELF_INTERSECTION_FIX_FAILED")
                                : std::string("IMPORT_OPEN_BOUNDARY_FIX_FAILED"),
                            (code == "IMPORT_SELF_INTERSECTION" ||
                             code == "IMPORT_GEOM_OPENBOUNDARY_SELF_INTERSECTION")
                                ? std::string("Fix could not split this self-intersection into separate solids. "
                                              "Try repairing in CAD, then re-import.")
                                : std::string("Fix could not infer a closed linear boundary loop for this mesh. "
                                              "Try repairing in your CAD tool, then re-import."),
                            std::string("")});
                        importOpenBoundaryBannerDismissed = false;
                        SyncCalibrateImportPrerequisiteVisibility();
                        uiRenderer.MarkDirty();
                        renderDirty = true;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Exit##oobExit"))
                {
                    importOpenBoundaryBannerDismissed = true;
                    SyncCalibrateImportPrerequisiteVisibility();
                    uiRenderer.MarkDirty();
                    renderDirty = true;
                }
                const float totalH = (ImGui::GetItemRectMax().y - row0.y) + pad;
                ImGui::Dummy(ImVec2(w, totalH));
            };
        }

        // ── Paragraph pointers for live state mutation ──────────────────────
        Section *prereqs = FindSection(*uiCalibrate, "Prerequisites");
        calibSec_Prerequisites = prereqs;
        calibPara_Import = &prereqs->children[0];
        calibPara_ImportClosed = &prereqs->children[1];
        calibPara_Point1 = &prereqs->children[2];
        calibPara_Point2 = &prereqs->children[3];
        calibLine_Point1Primary = &prereqs->children[2].values[0];
        calibLine_Point2Primary = &prereqs->children[3].values[0];

        calibSec_Parameters = FindSection(*uiCalibrate, "Parameters");
        calibSec_Result = FindSection(*uiCalibrate, "Calculator");
        if (calibSec_Parameters && !calibSec_Parameters->children.empty())
        {
            calibSec_Parameters->noChildSplitters = false;
            for (Paragraph &child : calibSec_Parameters->children)
                child.margin = UIGrid::GAP * UIElement::INSET_RATIO * 0.5f;
            calibPara_Measure = &calibSec_Parameters->children[0];
        }
        else
        {
            auto findRootParagraph = [this](const std::string &id) -> Paragraph *
            {
                for (ChildElement &child : uiCalibrate->children)
                {
                    if (Paragraph *p = std::get_if<Paragraph>(&child); p && p->id == id)
                        return p;
                }
                return nullptr;
            };
            calibPara_Measure = findRootParagraph("CalibMeasure");
            if (calibPara_Measure)
                calibPara_Measure->margin = UIGrid::GAP * UIElement::INSET_RATIO * 0.5f;
        }

        if (calibSec_Result)
        {
            calibSec_Result->noChildSplitters = false;
            Paragraph &derivedPara = calibSec_Result->AddParagraph("CalibDerived");
            derivedPara.values.reserve(1);
            derivedPara.values.push_back(std::move(pmDer.line));
            calibPara_Derived = &derivedPara;
            calibPara_Derived->margin = UIGrid::GAP * UIElement::INSET_RATIO * 0.5f;
        }
        else
        {
            calibPara_Derived = nullptr;
        }

        // Click handlers — selecting a point prerequisite deselects the other.
        auto selectPoint1 = [this]()
        {
            calibPara_Point1->selected = true;
            calibPara_Point2->selected = false;
            uiRenderer.MarkDirty();
            MarkPickDirty();
        };
        auto selectPoint2 = [this]()
        {
            calibPara_Point2->selected = true;
            calibPara_Point1->selected = false;
            uiRenderer.MarkDirty();
            MarkPickDirty();
        };
        calibPara_Point1->onClick        = selectPoint1;
        calibLine_Point1Primary->onClick = selectPoint1;
        if (calibPara_Point1->values.size() > 1)
            calibPara_Point1->values[1].onClick = selectPoint1;
        calibPara_Point2->onClick        = selectPoint2;
        calibLine_Point2Primary->onClick = selectPoint2;
        if (calibPara_Point2->values.size() > 1)
            calibPara_Point2->values[1].onClick = selectPoint2;

        // Point1 and Point2 are hidden until a file is imported
        calibPara_Point1->visible = false;
        calibPara_Point2->visible = false;
        if (calibPara_ImportClosed)
            calibPara_ImportClosed->visible = false;
        if (calibSec_Parameters)
            calibSec_Parameters->visible = false;
        if (calibSec_Result)
            calibSec_Result->visible = false;
        if (calibPara_Measure)
            calibPara_Measure->visible = false;
        if (calibPara_Derived)
            calibPara_Derived->visible = false;

        RefreshCalibDerivedRowVisible();
    }

    {
        ToolPanelDef structDef;
        structDef.id = "Structure";
        structDef.name = "Structure";
        structDef.description = "Carve printable patterns into solid faces";
        structDef.flattenParameters = true;

        structDef.prerequisites.reserve(2);
        structDef.prerequisites.push_back({"StructImport", "Import a file", "",
                                           Icons::CheckBox(&calibStepImport), false, true,
                                           [this]()
                                           { DoFileImport(); }});
        structDef.prerequisites.push_back({"StructImportClosed", "Geometry clean (tools)",
                                           "Required before face selection.",
                                           Icons::CheckBox(&calibStepImportClosedVolume), false, true, {}});

        structDef.optionalPrerequisites.reserve(1);
        structDef.optionalPrerequisites.push_back(
            PrerequisiteDef{"StructOptFaceExclude", "Face exclusions",
                            "Click eligible faces in the view to omit them from preview and carve.",
                            Icons::LeadingDrawFn{}, false, true,
                            [this]()
                            {
                                structureOptFaceExcludeStep =
                                    (structureOptFaceExcludeStep == Icons::StepState::Active)
                                        ? Icons::StepState::Done
                                        : Icons::StepState::Active;
                                SyncStructureOptionalPrereqRowStyle();
                                uiRenderer.MarkDirty();
                            }});

        structDef.parameters.reserve(1);
        {
            ParameterDef &p = structDef.parameters.emplace_back();
            p.id = "StructInset";
            makeSettingsDrag(p.line, "Inset", structureInsetMm, 0.05f, 0.5f, 5.0f, "%.1f mm",
                             "##structinset",
                             [this]()
                             {
                                 if (IsStructureStagingActive())
                                 {
                                     CancelPendingStructureCarveJob();
                                     structureCarvePipelinePhase =
                                         StructureCarvePipelinePhase::LaunchPending;
                                     SyncStructurePanelDerivedVisibility();
                                     uiRenderer.MarkDirty();
                                     renderDirty = true;
                                 }
                                 else
                                 {
                                     BeginStructureStagingSession();
                                 }
                             });
        }

        // hasSceneEditFooter stays false here: BuildToolPanel() would append the footer as the panel's
        // last child immediately, but HoverHint/ToolError are added afterward (below) and
        // would end up stacked beneath it. The footer paragraph is built manually after those instead, so
        // Cancel/Accept always render as the true last (bottom-most) row in the panel.
        structDef.sceneEditFooter.id = "StructSceneEditFooter";
        structDef.sceneEditFooter.line.getMinContentWidthPx = [settingsBodyFont]() -> float
        {
            ImFont *f = settingsBodyFont;
            if (!f)
                f = ImGui::GetFont();
            const float pad = ImGui::GetStyle().FramePadding.x;
            constexpr float zoneInset = 2.0f; // matches DrawSceneEditDualPillRow / settings Select
            const float cancelW = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, "Cancel").x;
            const float acceptW = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, "Accept").x;
            return cancelW + acceptW + 4.0f * pad + 4.0f * zoneInset + 4.0f;
        };
        structDef.sceneEditFooter.line.imguiContent = [this, settingsBodyFont](float w, float h, float)
        {
            ImFont *rowFont = FontOrInteractiveRow(uiRenderer, settingsBodyFont);
            if (rowFont)
                ImGui::PushFont(rowFont);
            DrawSceneEditDualPillRow(w, h, rowFont, "Cancel", "Accept",
                                     [this]()
                                     { FinalizeStructureSceneToolSession(false); },
                                     [this]()
                                     { FinalizeStructureSceneToolSession(true); },
                                     3);
            if (rowFont)
                ImGui::PopFont();
        };

        RootPanel structPanel = BuildToolPanel(structDef);
        structPanel.visible = false;
        structPanel.leftAnchor = PanelAnchor{uiToolbar, PanelAnchor::Right};
        structPanel.topAnchor = PanelAnchor{uiFiles, PanelAnchor::Bottom};
        uiStructure = &uiRenderer.AddPanel(structPanel);

        // Reserve root slots for HoverHint + tool error + footer **before** any `FindSection`
        // pointers are taken, so `AddParagraph` does not reallocate `children` and invalidate cached
        // `Paragraph*`/`Section*`.
        uiStructure->children.reserve(uiStructure->children.size() + 3);

        // Allocate the hover-hint paragraph FIRST so subsequent bindings into `uiStructure->children`
        // are stable. `BuildToolPanel` reserves capacity for exactly the structural children it knows
        // about, so adding HoverHint later forces a vector reallocation that silently invalidates
        // every previously-cached child pointer (`structPara_SceneEditFooter`, the `Prerequisites`
        // section, etc.). Symptom of the bug: `SyncStructurePanelDerivedVisibility` writes its
        // visibility flags into freed heap memory, so the Cancel/Accept footer never actually hides
        // before import.
        structPara_HoverHint = &uiStructure->AddParagraph("StructHoverHint");
        structPara_HoverHint->visible = false;
        structPara_HoverHint->padding = UIGrid::GAP * UIElement::INSET_RATIO * 0.85f;
        structPara_HoverHint->values.reserve(1);
        {
            SectionLine &line = structPara_HoverHint->values.emplace_back();
            line.text = "";
            line.textDepth = 2;
        }

        structPara_ToolError = &uiStructure->AddParagraph("StructToolError");
        structPara_ToolError->visible = false;
        structPara_ToolError->padding = UIGrid::GAP * UIElement::INSET_RATIO * 0.85f;
        structPara_ToolError->values.reserve(1);
        {
            SectionLine &el = structPara_ToolError->values.emplace_back();
            el.imguiContent = [this, settingsBodyFont](float w, float h, float)
            {
                (void)h;
                if (!structureToolError.has_value())
                {
                    ImGui::Dummy(ImVec2(w, 1.f));
                    return;
                }
                ImFont *rowFont = FontOrInteractiveRow(uiRenderer, settingsBodyFont);
                const float pad = ImGui::GetStyle().FramePadding.x;
                const ImVec2 row0 = ImGui::GetCursorScreenPos();
                const ToolUserErrorPayload &te = *structureToolError;
                const float errH =
                    DrawToolUserErrorCopyBlock(row0.x, row0.y, w, pad, rowFont, te.code, te.message,
                                               te.relatedParameterLabel, "structErr");
                ImGui::Dummy(ImVec2(w, std::max(errH, 1.f) + pad));
            };
        }

        // Footer is appended last so Cancel/Accept always render at the bottom of the panel,
        // below HoverHint/ToolError.
        structPara_SceneEditFooter = &uiStructure->AddParagraph(structDef.sceneEditFooter.id);
        structPara_SceneEditFooter->values.reserve(1);
        structPara_SceneEditFooter->values.push_back(structDef.sceneEditFooter.line);

        if (Section *structPrereqs = FindSection(*uiStructure, "Prerequisites");
            structPrereqs != nullptr && !structPrereqs->children.empty())
        {
            structPara_Import = &structPrereqs->children[0];
            if (structPrereqs->children.size() > 1)
                structPara_ImportClosed = &structPrereqs->children[1];
        }
        if (Section *structOpt = FindSection(*uiStructure, "ExtraPrerequisites"); structOpt != nullptr)
        {
            for (Paragraph &p : structOpt->children)
            {
                if (p.id == "StructOptFaceExclude")
                {
                    structPara_OptionalFaceExclude = &p;
                    break;
                }
            }
        }
        SyncStructurePanelDerivedVisibility();
    }

    SyncToolbarToolVisualState();
}

void Display::ResetStructurePreviewIncrementalState()
{
    structurePreviewBakeQueue.clear();
    structurePreviewBakeCursor = 0;
    structurePreviewBakedSegments.clear();
    structurePreviewCutOutlineSegments.clear();
    structurePreviewNotchedSegments.clear();
    structurePreviewAllCandidatesSegments.clear();
    structurePreviewStrutQuadSegments.clear();
    structureDebugCutOutlineSnapshot.clear();
    structureDebugNotchedSnapshot.clear();
    structureDebugAllCandidatesSnapshot.clear();
    structureDebugStrutQuadSnapshot.clear();
    structureDebugFullSnapshot.clear();
    structurePreviewBakeInsetMm = std::numeric_limits<double>::quiet_NaN();
}

void Display::CollectStructurePreviewWorkOrder(std::vector<const Face *> &out) const
{
    out.clear();
    out.reserve(structureEligibleFacesCache.size());
    for (const Face *f : structureEligibleFacesCache)
    {
        if (structureExcludedFaces.count(f) > 0)
            continue;
        out.push_back(f);
    }
    std::sort(out.begin(), out.end());
}

bool Display::StructurePreviewBakeSnapshotMatches(const std::vector<const Face *> &sortedFaces,
                                                  double insetMm) const
{
    if (std::isnan(structurePreviewBakeInsetMm) || structurePreviewBakeInsetMm != insetMm)
        return false;
    if (sortedFaces.size() != structurePreviewBakeQueue.size())
        return false;
    for (std::size_t i = 0; i < sortedFaces.size(); ++i)
    {
        if (sortedFaces[i] != structurePreviewBakeQueue[i])
            return false;
    }
    return true;
}

void Display::AdvanceStructurePreviewBuild(double insetMm)
{
    const std::size_t n = structurePreviewBakeQueue.size();
    if (structurePreviewBakeCursor >= n)
        return;
    StructureTriangulation::BakeParams params;
    params.insetMm = insetMm;
    params.chordTolMm = 0.02;
    params.minFeatureMm = 1.5;
    const std::size_t end =
        std::min(n, structurePreviewBakeCursor + kStructurePreviewMaxFacesPerFrame);
    for (std::size_t i = structurePreviewBakeCursor; i < end; ++i)
    {
        const Face *f = structurePreviewBakeQueue[i];

        // Stage 0: base cut outline (no notches, no fillets).
        auto segs = StructureTriangulation::BuildCutOutlinePreviewLines(f, params);
        StructureTriangulation::AppendPreviewLineSegments(segs, structurePreviewCutOutlineSegments);

        // Stage 1: notched outline without ChFi2d fillets (rectangular strut corners).
        {
            auto notchedSegs = StructureTriangulation::BuildNotchedCutOutlinePreviewLines(f, params);
            StructureTriangulation::AppendPreviewLineSegments(notchedSegs, structurePreviewNotchedSegments);
        }

        // Stage 2: all valid strut centerlines before best-pair selection + base outline.
        StructureTriangulation::AppendPreviewLineSegments(segs, structurePreviewAllCandidatesSegments);
        {
            auto allCands = StructureTriangulation::BuildAllStrutCandidatePreviewLines(f, params);
            StructureTriangulation::AppendPreviewLineSegments(allCands, structurePreviewAllCandidatesSegments);
        }

        // Stage 4: unclipped strut quad outlines (debug — shows endpoints, miter, full extent).
        {
            auto quadSegs = StructureTriangulation::BuildStrutQuadPreviewLines(f, params);
            StructureTriangulation::AppendPreviewLineSegments(quadSegs, structurePreviewStrutQuadSegments);
        }

        // Stage 3: full preview — cut outline + strut rails + anchor markers.
        StructureTriangulation::AppendPreviewLineSegments(segs, structurePreviewBakedSegments);
        const auto strutRails = StructureTriangulation::BuildStrutRailPreviewLines(f, params);
        StructureTriangulation::AppendPreviewLineSegments(strutRails, structurePreviewBakedSegments);

        const auto anchors = StructureTriangulation::BuildStrutAnchorPreviewPoints(f, params);
        const float armLen = static_cast<float>(insetMm * 0.2f);
        for (const auto &a : anchors)
        {
            structurePreviewBakedSegments.push_back(
                {a.pos + glm::vec3(-armLen, 0.0f, 0.0f),
                 a.pos + glm::vec3( armLen, 0.0f, 0.0f)});
            structurePreviewBakedSegments.push_back(
                {a.pos + glm::vec3(0.0f, -armLen, 0.0f),
                 a.pos + glm::vec3(0.0f,  armLen, 0.0f)});
            structurePreviewBakedSegments.push_back(
                {a.pos,
                 a.pos + glm::vec3(a.N.x, a.N.y, 0.0f) * armLen * 1.5f});
        }
    }
    structurePreviewBakeCursor = end;
    if (structurePreviewBakeCursor >= structurePreviewBakeQueue.size())
    {
        structureDebugCutOutlineSnapshot      = structurePreviewCutOutlineSegments;
        structureDebugNotchedSnapshot         = structurePreviewNotchedSegments;
        structureDebugAllCandidatesSnapshot   = structurePreviewAllCandidatesSegments;
        structureDebugStrutQuadSnapshot       = structurePreviewStrutQuadSegments;
        structureDebugFullSnapshot            = structurePreviewBakedSegments;
    }
}

const std::vector<std::pair<glm::vec3, glm::vec3>> &Display::StageSegments(int stage) const
{
    switch (stage)
    {
    case 0:  return structurePreviewCutOutlineSegments;
    case 1:  return structurePreviewNotchedSegments;
    case 2:  return structurePreviewAllCandidatesSegments;
    case 4:  return structurePreviewStrutQuadSegments;
    default: return structurePreviewBakedSegments;
    }
}

const std::vector<std::pair<glm::vec3, glm::vec3>> &Display::StageSnapshot(int stage) const
{
    switch (stage)
    {
    case 0:  return structureDebugCutOutlineSnapshot;
    case 1:  return structureDebugNotchedSnapshot;
    case 2:  return structureDebugAllCandidatesSnapshot;
    case 3:  return structureDebugFullSnapshot;
    case 4:  return structureDebugStrutQuadSnapshot;
    default: return structurePreviewBakedSegments;
    }
}

void Display::TickStructurePreviewBuildIfNeeded()
{
    if (activeTool != ActiveTool::Structure || IsStructureStagingActive())
        return;
    // Avoid running preview bake concurrently with the carve worker on another thread.
    if (structureCarvePipelinePhase != StructureCarvePipelinePhase::Idle ||
        pendingStructureStagingTask.has_value())
        return;
    std::vector<const Face *> workOrder;
    CollectStructurePreviewWorkOrder(workOrder);
    const double inset = static_cast<double>(structureInsetMm);
    if (!StructurePreviewBakeSnapshotMatches(workOrder, inset))
    {
        structurePreviewBakeQueue = std::move(workOrder);
        structurePreviewBakeInsetMm = inset;
        structurePreviewBakeCursor = 0;
        structurePreviewBakedSegments.clear();
        structurePreviewCutOutlineSegments.clear();
        structurePreviewNotchedSegments.clear();
        structurePreviewAllCandidatesSegments.clear();
        structurePreviewStrutQuadSegments.clear();
    }
    if (structurePreviewBakeCursor >= structurePreviewBakeQueue.size())
        return;
    AdvanceStructurePreviewBuild(inset);
    renderer.SetStructurePreviewSegments(StageSegments(structureDebugPreviewStage));
    renderDirty = true;
    // Bake is capped per frame; re-arm the wake event while work remains so the blocking
    // SDL_WaitEventTimeout doesn't strand the rest of the bake until a real input event arrives.
    if (structurePreviewBakeCursor < structurePreviewBakeQueue.size())
        AppWakeEvent::Push();
}

void Display::RefreshStructurePreviewForRenderer()
{
    if (activeTool != ActiveTool::Structure || IsStructureStagingActive())
    {
        structurePreviewBakeQueue.clear();
        structurePreviewBakeCursor = 0;
        structurePreviewBakedSegments.clear();
        structurePreviewCutOutlineSegments.clear();
        structurePreviewNotchedSegments.clear();
        structurePreviewAllCandidatesSegments.clear();
        structurePreviewStrutQuadSegments.clear();
        structurePreviewBakeInsetMm = std::numeric_limits<double>::quiet_NaN();
        renderer.SetStructurePreviewSegments({});
        return;
    }
    if (structureCarvePipelinePhase != StructureCarvePipelinePhase::Idle ||
        pendingStructureStagingTask.has_value())
    {
        structurePreviewBakeQueue.clear();
        structurePreviewBakeCursor = 0;
        structurePreviewBakedSegments.clear();
        structurePreviewCutOutlineSegments.clear();
        structurePreviewNotchedSegments.clear();
        structurePreviewAllCandidatesSegments.clear();
        structurePreviewStrutQuadSegments.clear();
        structurePreviewBakeInsetMm = std::numeric_limits<double>::quiet_NaN();
        renderer.SetStructurePreviewSegments({});
        return;
    }

    std::vector<const Face *> workOrder;
    CollectStructurePreviewWorkOrder(workOrder);
    const double inset = static_cast<double>(structureInsetMm);
    if (!StructurePreviewBakeSnapshotMatches(workOrder, inset))
    {
        structurePreviewBakeQueue = std::move(workOrder);
        structurePreviewBakeInsetMm = inset;
        structurePreviewBakeCursor = 0;
        structurePreviewBakedSegments.clear();
        structurePreviewCutOutlineSegments.clear();
        structurePreviewNotchedSegments.clear();
        structurePreviewAllCandidatesSegments.clear();
        structurePreviewStrutQuadSegments.clear();
    }

    while (structurePreviewBakeCursor < structurePreviewBakeQueue.size())
        AdvanceStructurePreviewBuild(inset);

    renderer.SetStructurePreviewSegments(StageSegments(structureDebugPreviewStage));
}

void Display::ToggleStructureDebugCutOutlineMode()
{
    // Stages: 0=cut outline, 1=no fillet, 2=all candidates, 3=full, 4=strut quads
    // Stage 4 uses value 4 in StageSegments/Snapshot but cycles as position 4 in [0..4].
    static const int kStageOrder[5] = {0, 1, 2, 3, 4};
    static const char *const kLabels[5] = {
        " [cut outline]", " [no fillet]", " [all candidates]", " [full]", " [strut quads]"};
    const int pos = [&]() -> int {
        for (int i = 0; i < 5; ++i)
            if (kStageOrder[i] == structureDebugPreviewStage) return i;
        return 3;
    }();
    const int nextPos = (pos + 1) % 5;
    structureDebugPreviewStage = kStageOrder[nextPos];
    SetStructurePanelHeaderTrailing(uiStructure, uiRenderer, kLabels[nextPos]);
    uiRenderer.MarkDirty();
    // Push snapshot directly — do NOT call RefreshStructurePreviewForRenderer (clears live
    // segments when staging is active). Snapshots survive the carve phase.
    renderer.SetStructurePreviewSegments(StageSnapshot(structureDebugPreviewStage));
    renderDirty = true;
}

void Display::RefreshAnalysisDebugSlicedView()
{
    if (!analysisDebugViewEnabled || activeTool != ActiveTool::Analysis || scene == nullptr)
    {
        analysisDebugLayersBySolid.clear();
        analysisDebugLayerIndex = 0;
        analysisDebugSlicedSegments.clear();
        renderer.SetAnalysisDebugSegments({});
        renderer.SetAnalysisDebugLayerDiff({}, 0.0f);
        return;
    }

    analysisDebugLayersBySolid.clear();
    analysisDebugLayersBySolid.reserve(scene->solids.size());
    for (const Solid &solid : scene->solids)
        analysisDebugLayersBySolid.push_back(Analysis::Instance().SliceSolidForDebug(&solid));

    size_t maxLayerCount = 0;
    for (const auto &layers : analysisDebugLayersBySolid)
        maxLayerCount = std::max(maxLayerCount, layers.size());
    if (maxLayerCount == 0)
        analysisDebugLayerIndex = 0;
    else
        analysisDebugLayerIndex =
            std::clamp(analysisDebugLayerIndex, 0, static_cast<int>(maxLayerCount) - 1);

    RebuildAnalysisDebugSegmentsForCurrentLayer();
    UpdateAnalysisDebugHeaderTrailing();
}

void Display::RebuildAnalysisDebugSegmentsForCurrentLayer()
{
    analysisDebugSlicedSegments.clear();
    std::vector<std::vector<glm::dvec3>> diffRings;
    float diffZ = 0.0f;

    for (const auto &layers : analysisDebugLayersBySolid)
    {
        const size_t idx = static_cast<size_t>(analysisDebugLayerIndex);
        if (analysisDebugLayerIndex < 0 || idx >= layers.size())
            continue;
        const SlicedLayer &layer = layers[idx];
        for (const SliceLoop &loop : layer.loops)
        {
            const size_t n = loop.ring.size();
            for (size_t i = 0; i < n; ++i)
            {
                const glm::dvec3 &a = loop.ring[i];
                const glm::dvec3 &b = loop.ring[(i + 1) % n];
                analysisDebugSlicedSegments.emplace_back(glm::vec3(a), glm::vec3(b));
            }
        }

        if (idx > 0)
        {
            const SlicedLayer &prev = layers[idx - 1];
            const SlicedLayer &curr = layers[idx];
            diffZ = static_cast<float>(curr.z);

            auto currRings = LayerDiffUtils::ToClassifiedRings(curr.loops);
            auto prevRings = LayerDiffUtils::ToClassifiedRings(prev.loops);
            for (auto &r : currRings) r.isHole = false;
            for (auto &r : prevRings) r.isHole = false;
            const auto diff = GeometryOps::RingDifference(currRings, prevRings, curr.z, 0.05);

            for (const auto &cr : diff)
            {
                if (!cr.isHole && cr.ring.size() >= 3)
                    diffRings.push_back(cr.ring);
            }
        }
    }

    renderer.SetAnalysisDebugSegments(analysisDebugSlicedSegments);
    renderer.SetAnalysisDebugLayerDiff(diffRings, diffZ);
}

void Display::UpdateAnalysisDebugHeaderTrailing()
{
    if (!analysisDebugViewEnabled)
    {
        ClearStructurePanelHeaderTrailing(uiAnalysis, uiRenderer);
        return;
    }
    size_t maxLayerCount = 0;
    for (const auto &layers : analysisDebugLayersBySolid)
        maxLayerCount = std::max(maxLayerCount, layers.size());
    if (maxLayerCount == 0)
    {
        SetStructurePanelHeaderTrailing(uiAnalysis, uiRenderer, " [no layers]");
        return;
    }
    SetStructurePanelHeaderTrailing(
        uiAnalysis, uiRenderer,
        " [layer " + std::to_string(analysisDebugLayerIndex + 1) + "/" + std::to_string(maxLayerCount) + "]");
}

void Display::ToggleAnalysisDebugSlicedView()
{
    analysisDebugViewEnabled = !analysisDebugViewEnabled;
    if (analysisDebugViewEnabled)
        analysisDebugLayerIndex = 0;
    uiRenderer.MarkDirty();
    RefreshAnalysisDebugSlicedView();
    renderDirty = true;
}

void Display::StepAnalysisDebugLayer(int delta)
{
    if (!analysisDebugViewEnabled || activeTool != ActiveTool::Analysis)
        return;
    size_t maxLayerCount = 0;
    for (const auto &layers : analysisDebugLayersBySolid)
        maxLayerCount = std::max(maxLayerCount, layers.size());
    if (maxLayerCount == 0)
        return;
    const int newIndex = std::clamp(analysisDebugLayerIndex + delta, 0, static_cast<int>(maxLayerCount) - 1);
    if (newIndex == analysisDebugLayerIndex)
        return;
    analysisDebugLayerIndex = newIndex;
    RebuildAnalysisDebugSegmentsForCurrentLayer();
    UpdateAnalysisDebugHeaderTrailing();
    uiRenderer.MarkDirty();
    renderDirty = true;
}

void Display::CancelPendingStructureCarveJob()
{
    structureCarvePipelinePhase = StructureCarvePipelinePhase::Idle;
    structureToolError.reset();
    if (!pendingStructureStagingTask.has_value())
    {
        ClearStructurePanelHeaderTrailing(uiStructure, uiRenderer);
        return;
    }
    pendingStructureStagingTask->RequestCancel();
    pendingStructureStagingTask.reset();
    ++structureStagingIssuedJobId;
    ClearPendingStructureProgressSnapshot();
    ClearStructurePanelHeaderTrailing(uiStructure, uiRenderer);
    SyncStructurePanelDerivedVisibility();
    uiRenderer.MarkDirty();
    renderDirty = true;
}

void Display::PollStructureStagingTaskIfReady()
{
    if (!pendingStructureStagingTask.has_value())
        return;

    const bool pollTraceOn = []()
    {
        const char *e = std::getenv("CAD_STRUCTURE_POLL_TRACE");
        return e != nullptr && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
    }();

    std::optional<AsyncStructureStagingResult> ready;
    try
    {
        const std::chrono::milliseconds maxWait = WorkerFuturePollRemainingMs();
        const auto t0 = std::chrono::steady_clock::now();
        ready = pendingStructureStagingTask->TryTake(maxWait);
        const auto t1 = std::chrono::steady_clock::now();
        if (pollTraceOn)
        {
            const uint64_t waitedMs =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
            SessionLogger::Instance().LogStructureStagingPollSlice(
                static_cast<uint64_t>(maxWait.count()), waitedMs, ready.has_value());
        }
    }
    catch (...)
    {
        // Worker packaged_task threw; do not leave the panel stuck on "Carving…".
        pendingStructureStagingTask.reset();
        structureCarvePipelinePhase = StructureCarvePipelinePhase::Idle;
        ClearPendingStructureProgressSnapshot();
        SessionLogger::Instance().LogStructureStagingWorkerException();
        ClearStructurePanelHeaderTrailing(uiStructure, uiRenderer);
        structureToolError =
            MapStructureCarveRawToUserError("Structure carve failed (worker exception).");
        LOG_WARN("Structure staging: worker future threw; see session / terminal for details");
        SyncStructurePanelDerivedVisibility();
        uiRenderer.MarkDirty();
        renderDirty = true;
        return;
    }

    if (!ready.has_value())
        return;

    AsyncStructureStagingResult r = std::move(*ready);
    pendingStructureStagingTask.reset();
    structureCarvePipelinePhase = StructureCarvePipelinePhase::Idle;
    ClearPendingStructureProgressSnapshot();

    ApplyStructureStagingResult(std::move(r));
}

void Display::ApplyStructureStagingResult(AsyncStructureStagingResult &&r)
{
    struct FlushSessionAfterStructurePollScope
    {
        ~FlushSessionAfterStructurePollScope()
        {
            SessionLogger::Instance().MaybeFlushAfterStructurePoll();
        }
    } flushSessionAfterStructurePollScope;

    SessionLogger::Instance().LogStructureStagingWorkerResult(r.jobId, structureStagingIssuedJobId, r.cancelled,
                                                              r.carvedSolids, r.carveAttempts, static_cast<bool>(r.staging),
                                                              r.firstErr, r.targetSceneIndex);

    // As soon as the future is consumed, drop "Carving…" and refresh footer visibility (`structureCarveBusy`
    // is derived from `structureCarvePipelinePhase` + `pendingStructureStagingTask`). CGAL may still be printing to stderr while the worker
    // unwinds — without this, the header can lie behind stderr for a long time.
    ClearStructurePanelHeaderTrailing(uiStructure, uiRenderer);
    SyncStructurePanelDerivedVisibility();
    uiRenderer.MarkDirty();
    renderDirty = true;

    if (r.jobId != structureStagingIssuedJobId)
    {
        SessionLogger::Instance().LogStructureStagingApplyPhase(
            "discarded_stale_job", "result_job=" + std::to_string(r.jobId) +
                                       " issued_job=" + std::to_string(structureStagingIssuedJobId));
        return;
    }
    if (r.cancelled)
    {
        SessionLogger::Instance().LogStructureStagingApplyPhase("discarded_cancelled");
        return;
    }
    if (r.targetSceneIndex == SIZE_MAX || r.targetSceneIndex >= ownedScenes.size())
    {
        SessionLogger::Instance().LogStructureStagingApplyPhase(
            "discarded_bad_target_scene", "target_scene=" + std::to_string(r.targetSceneIndex));
        return;
    }
    if (r.targetSceneIndex != activeSceneIndex || activeTool != ActiveTool::Structure)
    {
        SessionLogger::Instance().LogStructureStagingApplyPhase(
            "discarded_wrong_tab_or_tool",
            "target_scene=" + std::to_string(r.targetSceneIndex) +
                " active_scene=" + std::to_string(activeSceneIndex));
        return;
    }
    if (!r.staging)
    {
        SessionLogger::Instance().LogStructureStagingApplyPhase("discarded_empty_staging");
        LOG_WARN("Structure staging: worker returned empty scene");
        return;
    }

    // All carve attempts failed: `TryApplyStructureCarve` left each solid unchanged, so `r.staging` is
    // equivalent to the pre-carve clone. Do not swap into staging mode — that would set
    // `structureOriginalScene` and block `BeginStructureStagingSession` / analysis until Accept or
    // Cancel even though nothing changed.
    if (r.carveAttempts > 0 && r.carvedSolids == 0)
    {
        ClearStructurePanelHeaderTrailing(uiStructure, uiRenderer);
        structureToolError = MapStructureCarveRawToUserError(r.firstErr);
        // Main-thread WARN writes can block on a backpressured terminal. On this path we only need
        // diagnostics, not user-facing console noise; keep the app responsive while preserving
        // structured breadcrumbs in SessionLogger.
        Log::Background("Structure staging: all carve attempts failed; staying on original scene. " +
                        (r.firstErr.empty() ? std::string()
                                            : SanitizeMessageForSingleLineLog(r.firstErr)));
        StructureTriangulation::ClearBakeCache();
        structureEligibleFacesCache.clear();
        SyncStructurePanelDerivedVisibility();
        MarkPickDirty();
        uiRenderer.MarkDirty();
        renderDirty = true;
        SessionLogger::Instance().LogStructureStagingApplyPhase(
            "apply_all_carves_failed_on_scene",
            "carve_attempts=" + std::to_string(r.carveAttempts) +
                (r.firstErr.empty() ? std::string() : std::string(" err=") + r.firstErr.substr(0, 120)));
        return;
    }

    LOG_DESC("Structure staging: carved solids", std::to_string(r.carvedSolids), "of",
             std::to_string(r.carveAttempts));

    // Only capture the original on the first carve; on re-carves (inset changed while staging
    // is already active) the slot already holds the true original — overwriting it with the
    // previously carved scene would corrupt the base for every subsequent re-carve.
    if (!structureOriginalScene)
        structureOriginalScene = std::move(ownedScenes[activeSceneIndex]);
    structureStagingSceneIndex = activeSceneIndex;
    ownedScenes[activeSceneIndex] = std::move(r.staging);
    scene = ownedScenes[activeSceneIndex].get();
    analysisUiScene = scene;
    // Every carve (including re-carves on inset change) produces new Face* identities, so the
    // carved-to-original lineage map must be rebuilt every time, not just on first capture.
    RebuildStructureCarvedToOriginalMap();

    if (!r.firstErr.empty())
    {
        ClearStructurePanelHeaderTrailing(uiStructure, uiRenderer);
        structureToolError = MapStructureCarveRawToUserError(r.firstErr);
        if (r.carvedSolids < r.carveAttempts)
            LOG_WARN("Structure staging: partial carve;", SanitizeMessageForSingleLineLog(r.firstErr));
    }
    else
    {
        ClearStructurePanelHeaderTrailing(uiStructure, uiRenderer);
        structureToolError.reset();
    }

    StructureTriangulation::ClearBakeCache();
    structureEligibleFacesCache.clear();
    SessionLogger::Instance().LogStructureStagingApplyPhase(
        "applied_scene_swap", "job=" + std::to_string(r.jobId) + " scene=" + std::to_string(activeSceneIndex));
    SessionLogger::Instance().MaybeFlushAfterStructurePoll();
    UpdateScene();
    SessionLogger::Instance().LogStructureStagingApplyPhase(
        "applied_after_update_scene", "job=" + std::to_string(r.jobId));
    SyncStructurePanelDerivedVisibility();
    MarkPickDirty();
    uiRenderer.MarkDirty();
    renderDirty = true;
}

void Display::FlushPendingStructureStagingCarveLaunchIfAny()
{
    if (StructureTriangulation::StructureStopsAtCutOutline())
        return;
    if (structureCarvePipelinePhase != StructureCarvePipelinePhase::LaunchPending)
        return;
    structureCarvePipelinePhase = StructureCarvePipelinePhase::Idle;
    if (activeTool != ActiveTool::Structure)
    {
        ClearStructurePanelHeaderTrailing(uiStructure, uiRenderer);
        SyncStructurePanelDerivedVisibility();
        uiRenderer.MarkDirty();
        renderDirty = true;
        return;
    }
    if (activeSceneIndex == SIZE_MAX || activeSceneIndex >= ownedScenes.size())
    {
        ClearStructurePanelHeaderTrailing(uiStructure, uiRenderer);
        SyncStructurePanelDerivedVisibility();
        uiRenderer.MarkDirty();
        renderDirty = true;
        return;
    }
    if (scene == nullptr || (scene->solids.empty() && scene->faces.empty()))
    {
        ClearStructurePanelHeaderTrailing(uiStructure, uiRenderer);
        SyncStructurePanelDerivedVisibility();
        uiRenderer.MarkDirty();
        renderDirty = true;
        return;
    }
    LaunchStructureStagingCarveJob();
}

void Display::LaunchStructureStagingCarveJob()
{
    if (StructureTriangulation::StructureStopsAtCutOutline())
        return;
    std::unordered_map<const Face *, Face *> structureFaceCloneRemap;
    // Always carve from the original unmodified scene so that re-carves with a changed inset
    // don't stack on top of a previously carved staging scene.
    Scene *baseScene = structureOriginalScene ? structureOriginalScene.get() : scene;
    std::unique_ptr<Scene> staging = baseScene->Clone(&structureFaceCloneRemap);
    if (!staging)
    {
        LOG_WARN("Structure staging: scene clone failed");
        ClearStructurePanelHeaderTrailing(uiStructure, uiRenderer);
        structureToolError = MapStructureCarveRawToUserError("Scene clone failed.");
        SyncStructurePanelDerivedVisibility();
        uiRenderer.MarkDirty();
        renderDirty = true;
        return;
    }

    std::unordered_set<const Face *> stagingCarveExcluded;
    stagingCarveExcluded.reserve(structureExcludedFaces.size());
    for (const Face *ef : structureExcludedFaces)
    {
        if (ef == nullptr)
            continue;
        auto it = structureFaceCloneRemap.find(ef);
        if (it != structureFaceCloneRemap.end())
            stagingCarveExcluded.insert(it->second);
    }

    StructureTriangulation::BakeParams params;
    params.insetMm = static_cast<double>(structureInsetMm);
    params.chordTolMm = 0.02;
    params.minFeatureMm = 1.5;

    std::unordered_map<Solid *, std::vector<const Face *>> bySolid;
    // Excluded-but-eligible faces of each carved solid: passed through to the carve so the post-cut
    // unify pass doesn't merge their untouched geometry into a carved neighbor (see
    // `TryApplyStructureCarve`'s `keepFaces` doc).
    std::unordered_map<Solid *, std::vector<const Face *>> excludedBySolid;
    size_t eligibleCount = 0;
    for (Face &f : staging->faces)
    {
        if (f.dependency == nullptr)
            continue;
        if (!IsStructureFaceEligible(&f))
            continue;
        if (stagingCarveExcluded.count(&f) > 0)
        {
            excludedBySolid[f.dependency].push_back(&f);
            continue;
        }
        bySolid[f.dependency].push_back(&f);
        ++eligibleCount;
    }
    LOG_DESC("Structure staging: eligible faces", std::to_string(eligibleCount), "across solids",
             std::to_string(bySolid.size()));

    // `eligibleCount == 0` alone doesn't mean an error — the user may have excluded every eligible
    // face, which is a valid state (nothing left to cut, show the model untouched). Only bail when
    // there are no eligible faces at all, excluded or not.
    if (eligibleCount == 0 && excludedBySolid.empty())
    {
        ClearStructurePanelHeaderTrailing(uiStructure, uiRenderer);
        structureToolError = MapStructureCarveRawToUserError(
            "No eligible upward planar faces found for Structure carve.");
        SyncStructurePanelDerivedVisibility();
        uiRenderer.MarkDirty();
        renderDirty = true;
        return;
    }

    // Bake debug snapshots from the pre-carve faces now, before staging clears the live segments.
    // This lets ` cycle through debug stages even after the carve completes.
    {
        structureDebugCutOutlineSnapshot.clear();
        structureDebugNotchedSnapshot.clear();
        structureDebugAllCandidatesSnapshot.clear();
        structureDebugStrutQuadSnapshot.clear();
        structureDebugFullSnapshot.clear();
        for (const auto &[solid, faces] : bySolid)
        {
            for (const Face *f : faces)
            {
                auto outline = StructureTriangulation::BuildCutOutlinePreviewLines(f, params);
                StructureTriangulation::AppendPreviewLineSegments(outline, structureDebugCutOutlineSnapshot);

                auto notched = StructureTriangulation::BuildNotchedCutOutlinePreviewLines(f, params);
                StructureTriangulation::AppendPreviewLineSegments(notched, structureDebugNotchedSnapshot);

                StructureTriangulation::AppendPreviewLineSegments(outline, structureDebugAllCandidatesSnapshot);
                auto allCands = StructureTriangulation::BuildAllStrutCandidatePreviewLines(f, params);
                StructureTriangulation::AppendPreviewLineSegments(allCands, structureDebugAllCandidatesSnapshot);

                auto quadSegs = StructureTriangulation::BuildStrutQuadPreviewLines(f, params);
                StructureTriangulation::AppendPreviewLineSegments(quadSegs, structureDebugStrutQuadSnapshot);

                StructureTriangulation::AppendPreviewLineSegments(outline, structureDebugFullSnapshot);
                auto rails = StructureTriangulation::BuildStrutRailPreviewLines(f, params);
                StructureTriangulation::AppendPreviewLineSegments(rails, structureDebugFullSnapshot);
            }
        }
    }

    const uint64_t jobId = ++structureStagingIssuedJobId;
    const size_t targetSceneIndex = activeSceneIndex;
    const size_t solidCarveGroups = bySolid.size();
    structureStagingStepsTotal = solidCarveGroups;
    structureStagingStepsDone.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(structureProgressMutex);
        structureProgressJobId = jobId;
        latestStructureProgressPhase.clear();
        latestStructureProgress01 = -1.0f;
        latestStructureProgressDirty = false;
    }
    structureProgressPhase.clear();
    structureProgress01 = -1.0f;

    structureToolError.reset();
    SyncStructurePanelDerivedVisibility();
    uiRenderer.MarkDirty();
    renderDirty = true;

    SessionLogger::Instance().LogStructureStagingJobSubmitted(jobId, targetSceneIndex, solidCarveGroups,
                                                               eligibleCount);

    structureCarvePipelinePhase = StructureCarvePipelinePhase::Carving;
    pendingStructureStagingTask = taskRunner->Submit(
        [this, jobId, targetSceneIndex, staging = std::move(staging), bySolid = std::move(bySolid),
         excludedBySolid = std::move(excludedBySolid),
         params](const TaskRunner::CancellationToken &) mutable -> AsyncStructureStagingResult
        {
            AsyncStructureStagingResult out;
            out.jobId = jobId;
            out.targetSceneIndex = targetSceneIndex;
            out.staging = std::move(staging);
            if (out.staging)
            {
                SessionLogger::Instance().LogStructureStagingWorkerStarted(jobId);

                size_t carvedSolids = 0;
                size_t carveAttempts = 0;
                std::string firstErr;
                size_t solidLaneIndex = 0;
                for (auto &entry : bySolid)
                {
                    ++carveAttempts;
                    SessionLogger::Instance().LogStructureStagingWorkerSolidBegin(
                        jobId, solidLaneIndex,
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<void *>(entry.first))),
                        entry.second.size());
                    std::string err;
                    const std::function<void(const std::string &)> carvePhaseTrace =
                        [jobId](const std::string &phase)
                    {
                        SessionLogger::Instance().LogStructureStagingWorkerCarvePhase(jobId, phase);
                    };
                    const ImportProgressCallback carveProgress = [this, jobId](const ImportProgress &p)
                    {
                        PublishStructureCarveProgress(jobId, p);
                    };
                    static const std::vector<const Face *> kNoKeepFaces;
                    auto keepIt = excludedBySolid.find(entry.first);
                    const bool tryOk = StructureCarve::TryApplyStructureCarve(
                        out.staging.get(), entry.first, entry.second, params, &err, nullptr, &carvePhaseTrace,
                        &carveProgress, keepIt != excludedBySolid.end() ? keepIt->second : kNoKeepFaces);
                    SessionLogger::Instance().LogStructureStagingWorkerSolidEnd(jobId, solidLaneIndex, tryOk);
                    if (tryOk)
                        ++carvedSolids;
                    else if (firstErr.empty() && !err.empty())
                        firstErr = err;
                    ++solidLaneIndex;
                    structureStagingStepsDone.store(solidLaneIndex, std::memory_order_relaxed);
                }

                out.carvedSolids = carvedSolids;
                out.carveAttempts = carveAttempts;
                if (carveAttempts > 0 && carvedSolids < carveAttempts && !firstErr.empty())
                    out.firstErr = (carvedSolids == 0) ? std::move(firstErr) : (std::string("Partial: ") + firstErr);

                SessionLogger::Instance().LogStructureStagingWorkerPackagingResult(
                    jobId, carvedSolids, carveAttempts, !out.firstErr.empty());
            }
            return out;
        });
}

void Display::BeginStructureStagingSession()
{
    if (IsStructureStagingActive())
        return;
    if (activeSceneIndex == SIZE_MAX || activeSceneIndex >= ownedScenes.size())
    {
        LOG_DESC("Structure staging skipped: no active imported tab");
        return;
    }
    if (scene == nullptr || (scene->solids.empty() && scene->faces.empty()))
    {
        LOG_DESC("Structure staging skipped: active scene is empty");
        return;
    }
    ClearStructurePanelHeaderTrailing(uiStructure, uiRenderer);
    structureOptFaceExcludeStep = Icons::StepState::Active;
    SyncStructureOptionalPrereqRowStyle();

    CancelPendingStructureCarveJob();

    if (StructureTriangulation::StructureStopsAtCutOutline())
    {
        LOG_DESC("Structure: cut-outline preview only (strip/fillet/carve disabled)");
        SyncStructurePanelDerivedVisibility();
        uiRenderer.MarkDirty();
        renderDirty = true;
        return;
    }

    structureCarvePipelinePhase = StructureCarvePipelinePhase::LaunchPending;
    SetStructurePanelHeaderTrailing(uiStructure, uiRenderer, "Preparing carve…");
    SyncStructurePanelDerivedVisibility();
    uiRenderer.MarkDirty();
    renderDirty = true;
}

void Display::RestoreStructureOriginalScene()
{
    CancelPendingStructureCarveJob();
    if (!IsStructureStagingActive())
        return;
    if (structureStagingSceneIndex < ownedScenes.size())
    {
        ownedScenes[structureStagingSceneIndex] = std::move(structureOriginalScene);
        if (activeSceneIndex == structureStagingSceneIndex)
            scene = ownedScenes[structureStagingSceneIndex].get();
    }
    structureOriginalScene.reset();
    structureCarvedToOriginal.clear();
    structureStagingSceneIndex = SIZE_MAX;
    analysisUiScene = scene;
    StructureTriangulation::ClearBakeCache();
    structureEligibleFacesCache.clear();
    UpdateScene();
    ClearStructurePanelHeaderTrailing(uiStructure, uiRenderer);
    structureToolError.reset();
    structureOptFaceExcludeStep = Icons::StepState::Active;
    SyncStructureOptionalPrereqRowStyle();
}

void Display::CommitStructureStagingScene()
{
    CancelPendingStructureCarveJob();
    if (!IsStructureStagingActive())
        return;
    // Exclusion picks reference faces in `structureOriginalScene`; drop them before destroying it.
    structureExcludedFaces.clear();
    structureEligibleFacesCache.clear();
    StructureTriangulation::ClearBakeCache();
    structureStagingSceneIndex = SIZE_MAX;
    structureOriginalScene.reset();
    structureCarvedToOriginal.clear();
    // Commit destroys the held original; `analysisUiScene` may still point at that scene — fix before any use.
    if (pendingAnalysisTask.has_value())
    {
        pendingAnalysisTask->RequestCancel();
        pendingAnalysisTask.reset();
        pendingAnalysisScene = nullptr;
    }
    pendingAnalysisTint.reset();
    activeAnalysisTintForRebuild.reset();
    activeAnalysisTintIdentityForRebuild = 0;
    lastCommittedAnalysisForRecolor.reset();
    analysisRequestId++;

    lastVerdictWasPass = false;
    flawOverhang = {};
    flawSharpCorner = {};
    flawInstability = {};
    flawLayerDifference = {};
    if (uiVerdict)
        uiVerdict->values.clear();
    analysisUiScene = scene;

    ClearStructurePanelHeaderTrailing(uiStructure, uiRenderer);
    structureToolError.reset();
    structureOptFaceExcludeStep = Icons::StepState::Active;
    SyncStructureOptionalPrereqRowStyle();
}

void Display::RebuildStructureStagingScene()
{
    if (!IsStructureStagingActive())
        return;
    // Re-carve in place, without going through `RestoreStructureOriginalScene()` +
    // `BeginStructureStagingSession()`: that round-trip briefly nulls `structureOriginalScene` (so
    // `IsStructureStagingActive()` reads false) until the next carve completes, and any face pick
    // that lands in that window resolves against the wrong branch in `TryCommitStructureFacePick`,
    // corrupting `structureExcludedFaces` with pointers that never match the ones already stored.
    // `LaunchStructureStagingCarveJob` already clones fresh from `structureOriginalScene` on every
    // call, so it's safe to re-launch directly and keep `structureOriginalScene` — and the `Face*`
    // identities `structureExcludedFaces`/`structureCarvedToOriginal` key on — intact throughout.
    CancelPendingStructureCarveJob();
    ClearStructurePanelHeaderTrailing(uiStructure, uiRenderer);
    if (StructureTriangulation::StructureStopsAtCutOutline())
    {
        SyncStructurePanelDerivedVisibility();
        uiRenderer.MarkDirty();
        renderDirty = true;
        return;
    }
    structureCarvePipelinePhase = StructureCarvePipelinePhase::LaunchPending;
    SetStructurePanelHeaderTrailing(uiStructure, uiRenderer, "Preparing carve…");
    SyncStructurePanelDerivedVisibility();
    uiRenderer.MarkDirty();
    renderDirty = true;
    MarkPickDirty();
}

void Display::FinalizeStructureSceneToolSession(bool accepted)
{
    if (activeTool != ActiveTool::Structure)
        return;
    if (accepted)
        CommitStructureStagingScene();
    else
        RestoreStructureOriginalScene();
    structureFinalizeCommitSkipGpuFullRebuild = accepted;
    structureOptFaceExcludeStep = Icons::StepState::Active;
    SyncStructureOptionalPrereqRowStyle();
    activeTool = ActiveTool::Analysis;
    pendingToolSwitch = true;
    renderDirty = true;
}

void Display::SyncStructurePanelDerivedVisibility()
{
    if (uiStructure == nullptr)
        return;
    // Footer (Cancel/Accept) only makes sense while the *active tab* actually has an imported
    // model. `calibStepImport` is a session-level "ever imported anything" flag and stays Done
    // forever, so reusing it here would leak Cancel/Accept onto an empty base scene tab.
    const bool activeHasModel =
        scene != nullptr && (!scene->solids.empty() || !scene->faces.empty()) &&
        !pendingImportTabActive;
    const bool structureCarveBusy = structureCarvePipelinePhase != StructureCarvePipelinePhase::Idle ||
                                    pendingStructureStagingTask.has_value();
    const bool importContractNeedsAttention =
        activeHasModel && calibStepImport == Icons::StepState::Done &&
        calibStepImportClosedVolume != Icons::StepState::Done;
    if (Section *prereq = FindSection(*uiStructure, "Prerequisites"))
        prereq->visible = !activeHasModel || importContractNeedsAttention;
    if (Section *optPre = FindSection(*uiStructure, "ExtraPrerequisites"))
        optPre->visible = activeHasModel && !importContractNeedsAttention;
    if (structPara_Import)
        structPara_Import->visible = !activeHasModel;
    if (structPara_ImportClosed)
        structPara_ImportClosed->visible = importContractNeedsAttention;
    const bool cutOutlineOnly = StructureTriangulation::StructureStopsAtCutOutline();
    if (structPara_SceneEditFooter)
        structPara_SceneEditFooter->visible = activeHasModel && !structureCarveBusy &&
                                              !importContractNeedsAttention && !cutOutlineOnly;
    const bool showStructureToolError =
        activeHasModel && structureToolError.has_value() && (activeTool == ActiveTool::Structure);
    if (structPara_ToolError)
    {
        structPara_ToolError->visible = showStructureToolError;
        if (!structPara_ToolError->values.empty())
            structPara_ToolError->values[0].visible = showStructureToolError;
    }
    if (structPara_HoverHint)
    {
        // Clickability hint deliberately suppressed at this stage — the algorithm is still in flux
        // and the per-face eligibility feedback is more noise than signal. Leaving the paragraph
        // and its plumbing in place so we can re-enable it later by flipping one line.
        structPara_HoverHint->visible = false;
    }
    SyncStructureOptionalPrereqRowStyle();
    uiRenderer.MarkDirty();
}

bool Display::ImportAllowsGeometryDependentTools() const noexcept
{
    return calibStepImport == Icons::StepState::Done &&
           calibStepImportClosedVolume == Icons::StepState::Done;
}

namespace
{
bool SplitSolidIntoFaceConnectedComponents(Scene *scene, Solid *solid, std::vector<Solid *> &outNewSolids)
{
    outNewSolids.clear();
    if (scene == nullptr || solid == nullptr || solid->faces.size() < 2u)
        return false;

    std::unordered_map<const Face *, int> faceIndex;
    faceIndex.reserve(solid->faces.size());
    for (std::size_t i = 0; i < solid->faces.size(); ++i)
    {
        Face *f = solid->faces[i];
        if (f != nullptr)
            faceIndex[f] = static_cast<int>(i);
    }
    if (faceIndex.size() < 2u)
        return false;

    std::unordered_map<const Edge *, std::vector<int>> edgeFaces;
    edgeFaces.reserve(solid->faces.size() * 3u + 8u);
    for (std::size_t i = 0; i < solid->faces.size(); ++i)
    {
        Face *f = solid->faces[i];
        if (f == nullptr)
            continue;
        for (const std::vector<OrientedEdge> &loop : f->loops)
        {
            for (const OrientedEdge &oe : loop)
            {
                if (oe.edge != nullptr)
                    edgeFaces[oe.edge].push_back(static_cast<int>(i));
            }
        }
    }

    std::vector<std::vector<int>> adjacency(solid->faces.size());
    for (const auto &kv : edgeFaces)
    {
        const std::vector<int> &ids = kv.second;
        for (std::size_t a = 0; a < ids.size(); ++a)
        {
            for (std::size_t b = a + 1u; b < ids.size(); ++b)
            {
                const int ia = ids[a];
                const int ib = ids[b];
                if (ia >= 0 && ib >= 0 && static_cast<std::size_t>(ia) < adjacency.size() &&
                    static_cast<std::size_t>(ib) < adjacency.size())
                {
                    adjacency[ia].push_back(ib);
                    adjacency[ib].push_back(ia);
                }
            }
        }
    }

    std::vector<char> visited(solid->faces.size(), 0);
    std::vector<std::vector<Face *>> components;
    for (std::size_t i = 0; i < solid->faces.size(); ++i)
    {
        if (visited[i] || solid->faces[i] == nullptr)
            continue;
        std::vector<Face *> comp;
        std::queue<int> q;
        q.push(static_cast<int>(i));
        visited[i] = 1;
        while (!q.empty())
        {
            const int idx = q.front();
            q.pop();
            Face *f = solid->faces[idx];
            if (f != nullptr)
                comp.push_back(f);
            for (int n : adjacency[idx])
            {
                if (n >= 0 && static_cast<std::size_t>(n) < visited.size() && !visited[n])
                {
                    visited[n] = 1;
                    q.push(n);
                }
            }
        }
        if (!comp.empty())
            components.push_back(std::move(comp));
    }

    if (components.size() <= 1u)
        return false;

    solid->faces = components[0];
    for (Face *f : solid->faces)
    {
        if (f != nullptr)
            f->dependency = solid;
    }
    GeometryValidity::InvalidateSolidAppGeometryValidityCache(*solid);
    GeometryValidity::RefreshSolidAppGeometryValidityCache(*solid);

    std::vector<Solid *> members;
    members.reserve(components.size());
    members.push_back(solid);
    for (std::size_t ci = 1; ci < components.size(); ++ci)
    {
        // Preserve raw topology for split pieces as well; generic solid-repair passes can remove
        // faces in non-manifold/self-intersection regions and make a failed split look destructive.
        Solid *ns = scene->CreateSolid(components[ci], false);
        if (ns != nullptr)
        {
            outNewSolids.push_back(ns);
            members.push_back(ns);
        }
    }
    if (members.size() > 1u)
        (void)scene->CreateCompound(std::move(members));
    return !outNewSolids.empty();
}
} // namespace

bool Display::TryFixSelfIntersectionForActiveScene()
{
    if (scene == nullptr || scene->solids.empty())
        return false;

    bool anySplit = false;
    const std::size_t initialSolidCount = scene->solids.size();
    for (std::size_t i = 0; i < initialSolidCount; ++i)
    {
        Solid *solid = &scene->solids[i];
        if (!solid->cachedAppInvalidGeometryTagsFresh)
            GeometryValidity::RefreshSolidAppGeometryValidityCache(*solid);
        if (!GeometryValidity::Any(solid->cachedAppInvalidGeometryTags &
                                   GeometryValidity::AppInvalidTag::SelfIntersection))
            continue;

        std::vector<Solid *> createdSplits;
        if (SplitSolidIntoFaceConnectedComponents(scene, solid, createdSplits))
            anySplit = true;
    }

    if (!anySplit)
        return false;

    for (Solid &solid : scene->solids)
        GeometryValidity::RefreshSolidAppGeometryValidityCache(solid);

    importOpenBoundaryBannerDismissed = false;
    RefreshImportClosedVolumeContractFromScene();
    ClearPickHover();
    ClearCalibrateFacePicks();
    ClearStructureFacePick();
    SyncCalibrateImportPrerequisiteVisibility();
    SyncStructurePanelDerivedVisibility();
    uiRenderer.MarkDirty();
    MarkGeometryDirtyAll();
    MarkPickDirty();
    renderDirty = true;
    return true;
}

bool Display::TryFixOpenBoundaryForActiveScene()
{
    if (scene == nullptr || scene->solids.empty())
        return false;

    bool madeAnyRepair = false;
    std::size_t skippedBoundaryEdges = 0;
    std::size_t loopsDetected = 0;
    std::size_t loopsCapped = 0;
    std::size_t hullFallbackCaps = 0;
    for (Solid &solid : scene->solids)
    {
        bool repairedThisSolid = false;
        std::vector<Face *> createdCapsThisSolid;
        std::vector<const Edge *> boundaryEdges;
        GeometryValidity::CollectOpenBoundaryEdgesForSolid(solid, boundaryEdges);
        if (boundaryEdges.empty())
            continue;
        std::vector<Edge *> repairCandidates;
        std::size_t skippedNonLinear = 0;
        CollectRepairCandidateLinearEdgesForSolid(solid, repairCandidates, skippedNonLinear);
        if (repairCandidates.empty())
        {
            skippedBoundaryEdges += skippedNonLinear;
            continue;
        }

        std::vector<std::vector<Edge *>> loopsToCap;
        std::size_t skippedThisSolid = 0;
        CollectClosedLinearBoundaryLoops(repairCandidates, loopsToCap, skippedThisSolid);
        loopsDetected += loopsToCap.size();
        skippedBoundaryEdges += skippedThisSolid + skippedNonLinear;

        if (loopsToCap.empty())
        {
            std::vector<Edge *> hullLoop;
            if (TryBuildPlanarHullCapLoop(scene, boundaryEdges, hullLoop) &&
                !SolidAlreadyHasEquivalentFaceLoops(solid, {hullLoop}))
            {
                Face *cap = scene->CreateFace({hullLoop});
                if (cap != nullptr)
                {
                    cap->dependency = &solid;
                    solid.faces.push_back(cap);
                    createdCapsThisSolid.push_back(cap);
                    repairedThisSolid = true;
                    madeAnyRepair = true;
                    loopsCapped++;
                    hullFallbackCaps++;
                }
            }
        }
        else
        {
            const std::vector<std::vector<std::vector<Edge *>>> groupedFaces = GroupCapLoopsIntoFaces(loopsToCap);
            for (const std::vector<std::vector<Edge *>> &faceLoops : groupedFaces)
            {
                if (faceLoops.empty() || SolidAlreadyHasEquivalentFaceLoops(solid, faceLoops))
                    continue;
                Face *cap = scene->CreateFace(faceLoops);
                if (cap == nullptr)
                    continue;
                cap->dependency = &solid;
                solid.faces.push_back(cap);
                createdCapsThisSolid.push_back(cap);
                repairedThisSolid = true;
                madeAnyRepair = true;
                loopsCapped++;
            }
        }

        PruneStandaloneInnerFixCaps(solid, createdCapsThisSolid);

        if (repairedThisSolid)
            GeometryValidity::InvalidateSolidAppGeometryValidityCache(solid);
    }

    if (!madeAnyRepair)
        return false;

    for (Solid &solid : scene->solids)
        scene->MergeCoplanarFaces(&solid);
    for (Solid &solid : scene->solids)
        GeometryValidity::RefreshSolidAppGeometryValidityCache(solid);

    importOpenBoundaryBannerDismissed = false;
    RefreshImportClosedVolumeContractFromScene();
    if (importOpenBoundaryToolPayload.has_value() && skippedBoundaryEdges > 0)
    {
        importOpenBoundaryToolPayload.emplace(ToolUserErrorPayload{
            std::string("IMPORT_OPEN_BOUNDARY_FIX_PARTIAL"),
            std::string("Fix capped ") + std::to_string(loopsCapped) + " loop(s) from " + std::to_string(loopsDetected) +
                " detected; skipped edges: " + std::to_string(skippedBoundaryEdges) +
                (hullFallbackCaps > 0 ? std::string(" (including planar hull fallback).")
                                      : std::string(". Some open boundaries remain unresolved.")),
            std::string("")});
    }
    LOG_DESC("OpenBoundary Fix:",
             std::string("loopsDetected=") + std::to_string(loopsDetected),
             std::string("loopsCapped=") + std::to_string(loopsCapped),
             std::string("hullFallbackCaps=") + std::to_string(hullFallbackCaps),
             std::string("skippedEdges=") + std::to_string(skippedBoundaryEdges));
    SyncCalibrateImportPrerequisiteVisibility();
    uiRenderer.MarkDirty();
    MarkGeometryDirtyAll();
    MarkPickDirty();
    renderDirty = true;
    return true;
}

void Display::RebuildOpenBoundaryBlameLineGpuMesh()
{
    openBoundaryBlameLineVertices.clear();
    openBoundaryBlameLineIndices.clear();
    if (!importOpenBoundaryBlameEdges.empty())
    {
        const glm::vec3 obRgb = glm::vec3(Color::GetAccentSteps(0.85f, 1.2f, 1.15f));
        for (const Edge *e : importOpenBoundaryBlameEdges)
            AppendOpenBoundaryBlameEdgeGeometry(e, obRgb, openBoundaryBlameLineVertices, openBoundaryBlameLineIndices);
    }
    renderer.UploadOpenBoundaryBlameLineMesh(openBoundaryBlameLineVertices, openBoundaryBlameLineIndices);
}

void Display::RebuildOpenBoundaryBlameFaceGpuMesh()
{
    openBoundaryBlameFaceVertices.clear();
    openBoundaryBlameFaceIndices.clear();
    if (scene == nullptr || calibStepImportClosedVolume == Icons::StepState::Done)
    {
        renderer.UploadOpenBoundaryBlameFaceMesh(openBoundaryBlameFaceVertices, openBoundaryBlameFaceIndices);
        return;
    }

    const glm::vec3 obRgb = glm::vec3(Color::GetAccentSteps(0.8f, 1.12f, 0.95f));
    for (const Solid &solid : scene->solids)
    {
        std::vector<const Edge *> boundaryEdges;
        GeometryValidity::CollectOpenBoundaryEdgesForSolid(solid, boundaryEdges);
        if (boundaryEdges.empty())
            continue;

        std::vector<Edge *> repairCandidates;
        std::size_t skippedNonLinear = 0;
        CollectRepairCandidateLinearEdgesForSolid(solid, repairCandidates, skippedNonLinear);

        std::vector<std::vector<Edge *>> loopsToCap;
        if (!repairCandidates.empty())
        {
            std::size_t skippedLoops = 0;
            CollectClosedLinearBoundaryLoops(repairCandidates, loopsToCap, skippedLoops);
        }
        if (loopsToCap.empty())
        {
            std::vector<Edge *> hullLoop;
            if (TryBuildPlanarHullCapLoop(scene, boundaryEdges, hullLoop, false))
                loopsToCap.push_back(std::move(hullLoop));
        }
        if (loopsToCap.empty())
        {
            std::unordered_set<const Point *> boundaryVertices;
            boundaryVertices.reserve(boundaryEdges.size() * 2u + 8u);
            for (const Edge *e : boundaryEdges)
            {
                if (e == nullptr)
                    continue;
                if (e->startPoint != nullptr)
                    boundaryVertices.insert(e->startPoint);
                if (e->endPoint != nullptr)
                    boundaryVertices.insert(e->endPoint);
            }
            std::vector<const Edge *> boundaryPlusContext = boundaryEdges;
            boundaryPlusContext.reserve(boundaryEdges.size() + 32u);
            std::unordered_set<const Edge *> seen(boundaryEdges.begin(), boundaryEdges.end());
            for (Face *face : solid.faces)
            {
                if (face == nullptr || face->loops.empty())
                    continue;
                for (const std::vector<OrientedEdge> &loop : face->loops)
                {
                    for (const OrientedEdge &oe : loop)
                    {
                        const Edge *e = oe.edge;
                        const Point *start = oe.GetStart();
                        const Point *end = oe.GetEnd();
                        if (e == nullptr || start == nullptr || end == nullptr || seen.contains(e))
                            continue;
                        if (!boundaryVertices.contains(start) || !boundaryVertices.contains(end))
                            continue;
                        seen.insert(e);
                        boundaryPlusContext.push_back(e);
                    }
                }
            }
            std::vector<Edge *> hullLoop;
            if (TryBuildPlanarHullCapLoop(scene, boundaryPlusContext, hullLoop, false))
                loopsToCap.push_back(std::move(hullLoop));
        }
        if (loopsToCap.empty())
            continue;

        const std::vector<std::vector<std::vector<Edge *>>> groupedFaces = GroupCapLoopsIntoFaces(loopsToCap);
        for (const std::vector<std::vector<Edge *>> &faceLoops : groupedFaces)
            AppendOpenBoundaryBlameFaceFillGeometry(faceLoops, obRgb, openBoundaryBlameFaceVertices, openBoundaryBlameFaceIndices);
    }

    renderer.UploadOpenBoundaryBlameFaceMesh(openBoundaryBlameFaceVertices, openBoundaryBlameFaceIndices);
}

void Display::RebuildImportOpenBoundaryBlameEdges()
{
    importOpenBoundaryBlameEdges.clear();
    importOpenBoundaryContextEdges.clear();
    if (scene == nullptr || calibStepImportClosedVolume == Icons::StepState::Done)
    {
        RebuildOpenBoundaryBlameFaceGpuMesh();
        RebuildOpenBoundaryBlameLineGpuMesh();
        MarkPickDirty();
        return;
    }
    std::unordered_set<const Edge *> seen;
    std::unordered_set<const Point *> boundaryVertices;
    std::vector<const Edge *> chunk;
    chunk.reserve(64);
    for (const Solid &solid : scene->solids)
    {
        chunk.clear();
        GeometryValidity::CollectOpenBoundaryEdgesForSolid(solid, chunk);
        for (const Edge *e : chunk)
        {
            if (e != nullptr && seen.insert(e).second)
                importOpenBoundaryBlameEdges.push_back(e);
            if (e != nullptr)
            {
                if (e->startPoint != nullptr)
                    boundaryVertices.insert(e->startPoint);
                if (e->endPoint != nullptr)
                    boundaryVertices.insert(e->endPoint);
            }
        }
    }

    std::unordered_set<const Edge *> contextSeen;
    for (const Solid &solid : scene->solids)
    {
        for (Face *face : solid.faces)
        {
            if (face == nullptr || face->loops.empty())
                continue;
            for (const std::vector<OrientedEdge> &loop : face->loops)
            {
                for (const OrientedEdge &oe : loop)
                {
                    const Edge *e = oe.edge;
                    const Point *start = oe.GetStart();
                    const Point *end = oe.GetEnd();
                    if (e == nullptr || start == nullptr || end == nullptr || seen.contains(e))
                        continue;

                    const bool startBoundary = boundaryVertices.contains(start);
                    const bool endBoundary = boundaryVertices.contains(end);
                    if (!startBoundary || !endBoundary)
                        continue;
                    if (contextSeen.insert(e).second)
                        importOpenBoundaryContextEdges.push_back(e);
                }
            }
        }
    }
    RebuildOpenBoundaryBlameFaceGpuMesh();
    RebuildOpenBoundaryBlameLineGpuMesh();
    MarkPickDirty();
}

void Display::RefreshImportClosedVolumeContractFromScene() noexcept
{
    using GeometryValidity::AppInvalidTag;
    using GeometryValidity::Any;

    if (scene == nullptr || scene->solids.empty())
    {
        calibStepImportClosedVolume = Icons::StepState::Done;
        importOpenBoundaryToolPayload.reset();
        importOpenBoundaryBannerDismissed = false;
        RebuildImportOpenBoundaryBlameEdges();
        return;
    }

    bool anyOpen = false;
    bool anySelfIntersection = false;
    std::size_t obSolidsWithOpen = 0;
    std::size_t obGroupsOpen = 0;
    std::size_t obGroupsManifoldOpposite = 0;
    std::size_t obGroupsCount2Same = 0;
    std::size_t obGroupsCount2NonOpposite = 0;
    std::size_t obGroups3Plus = 0;
    std::size_t obHighlightedEdges = 0;
    for (Solid &solid : scene->solids)
    {
        if (!solid.cachedAppInvalidGeometryTagsFresh)
            GeometryValidity::RefreshSolidAppGeometryValidityCache(solid);
        if (Any(solid.cachedAppInvalidGeometryTags & AppInvalidTag::OpenBoundary))
        {
            anyOpen = true;
            ++obSolidsWithOpen;
        }
        if (Any(solid.cachedAppInvalidGeometryTags & AppInvalidTag::SelfIntersection))
            anySelfIntersection = true;

        const GeometryValidity::OpenBoundaryDebugStats dbg =
            GeometryValidity::CollectOpenBoundaryDebugStatsForSolid(solid);
        obGroupsOpen += dbg.edgeGroupsOpenBoundary;
        obGroupsManifoldOpposite += dbg.edgeGroupsManifoldOpposite;
        obGroupsCount2Same += dbg.edgeGroupsCount2SameDirection;
        obGroupsCount2NonOpposite += dbg.edgeGroupsCount2NonOpposite;
        obGroups3Plus += dbg.edgeGroupsNonManifold3Plus;
        obHighlightedEdges += dbg.highlightedEdgeCount;
    }

    {
        std::ostringstream ss;
        ss << scene->solids.size() << "|" << obSolidsWithOpen << "|" << obGroupsOpen << "|"
           << obGroupsManifoldOpposite << "|" << obGroupsCount2Same << "|"
           << obGroupsCount2NonOpposite << "|" << obGroups3Plus << "|" << obHighlightedEdges;
        const std::string sig = ss.str();
        if (sig != openBoundaryDiagLastSignature)
        {
            openBoundaryDiagLastSignature = sig;
            auto &sl = SessionLogger::Instance();
            sl.LogOpenBoundaryDiagnostics(
                sl.state.lastFilename,
                scene->solids.size(),
                obSolidsWithOpen,
                obGroupsOpen,
                obGroupsManifoldOpposite,
                obGroupsCount2Same,
                obGroupsCount2NonOpposite,
                obGroups3Plus,
                obHighlightedEdges);
            sl.MaybeFlushAfterImport();
        }
    }

    if (anyOpen || anySelfIntersection)
    {
        calibStepImportClosedVolume = Icons::StepState::Active;
        if (anyOpen && anySelfIntersection)
        {
            importOpenBoundaryToolPayload.emplace(ToolUserErrorPayload{
                std::string("IMPORT_GEOM_OPENBOUNDARY_SELF_INTERSECTION"),
                std::string("Open boundary and self-intersection detected. Tools that require clean solids stay "
                            "unavailable until repaired."),
                std::string("")});
        }
        else if (anyOpen)
        {
            importOpenBoundaryToolPayload.emplace(ToolUserErrorPayload{
                std::string("IMPORT_OPEN_BOUNDARY"),
                std::string("Open boundary — this solid is not a closed volume. Tools that need a watertight mesh "
                            "stay unavailable until you repair."),
                std::string("")});
        }
        else
        {
            importOpenBoundaryToolPayload.emplace(ToolUserErrorPayload{
                std::string("IMPORT_SELF_INTERSECTION"),
                std::string("Self intersection detected — tools that require clean solids stay unavailable until "
                            "you repair."),
                std::string("")});
        }
    }
    else
    {
        calibStepImportClosedVolume = Icons::StepState::Done;
        importOpenBoundaryToolPayload.reset();
        importOpenBoundaryBannerDismissed = false;
    }
    RebuildImportOpenBoundaryBlameEdges();
}

void Display::SyncCalibrateImportPrerequisiteVisibility()
{
    if (calibPara_ImportClosed)
        calibPara_ImportClosed->visible = calibStepImport == Icons::StepState::Done;
    if (calibPara_OpenBoundaryBanner)
    {
        const bool show = importOpenBoundaryToolPayload.has_value() && calibStepImport == Icons::StepState::Done &&
                           calibStepImportClosedVolume != Icons::StepState::Done &&
                           !importOpenBoundaryBannerDismissed;
        calibPara_OpenBoundaryBanner->visible = show;
        if (!calibPara_OpenBoundaryBanner->values.empty())
            calibPara_OpenBoundaryBanner->values[0].visible = show;
    }
    uiRenderer.MarkDirty();
}

void Display::SyncStructureOptionalPrereqRowStyle()
{
    if (structPara_OptionalFaceExclude == nullptr)
        return;
    const bool arm = structureOptFaceExcludeStep == Icons::StepState::Active;
    structPara_OptionalFaceExclude->selected = arm;
    structPara_OptionalFaceExclude->dimFill = !arm;
    if (!structPara_OptionalFaceExclude->values.empty())
        structPara_OptionalFaceExclude->values[0].textDepth = arm ? 2 : 1;
    if (structPara_OptionalFaceExclude->values.size() > 1)
        structPara_OptionalFaceExclude->values[1].textDepth = arm ? 1 : 0;
}

