#include "CalibDistanceType.hpp"

#include "GeometryExperiments.hpp"
#include "scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace CalibrateDistance
{
namespace
{

constexpr double kCapNormalAlignMinAbsDot = 0.985; // ~10° — cap face ∥ build direction

[[nodiscard]] glm::dvec3 NormalizeBuildDir(const glm::dvec3 &buildDirWorld)
{
    glm::dvec3 d = glm::normalize(buildDirWorld);
    if (!std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.z) || glm::length(d) < 1e-9)
        return glm::dvec3(0.0, 0.0, 1.0);
    return d;
}

[[nodiscard]] bool FaceCapParallelBuildDir(const Face *face, const glm::dvec3 &dirUnit)
{
    if (face == nullptr || !face->HasGeometry() || !face->GetSurface().IsPlanar())
        return false;

    glm::dvec3 n = glm::normalize(face->GetSurface().GetNormal());
    if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z) || glm::length(n) < 1e-12)
        return false;
    return std::abs(glm::dot(n, dirUnit)) >= kCapNormalAlignMinAbsDot;
}

[[nodiscard]] double SceneMinAlongBuildDir(const Scene &scene, const glm::dvec3 &dirUnit)
{
    double tMin = std::numeric_limits<double>::max();

    auto considerFace = [&](const Face *f)
    {
        if (f == nullptr || !f->HasGeometry())
            return;
        for (const auto &loop : f->loops)
        {
            for (const auto &oe : loop)
            {
                const glm::dvec3 p0 = oe.GetStartPosition();
                const glm::dvec3 p1 = oe.GetEndPosition();
                tMin = std::min(tMin, std::min(glm::dot(p0, dirUnit), glm::dot(p1, dirUnit)));
            }
        }
    };

    for (const Solid &solid : scene.solids)
    {
        for (const Face *f : solid.faces)
            considerFace(f);
    }
    for (const Face &f : scene.faces)
        considerFace(&f);

    return tMin;
}

struct PointPairHash
{
    std::size_t operator()(const std::pair<Point *, Point *> &p) const noexcept
    {
        return std::hash<Point *>{}(p.first) ^ (std::hash<Point *>{}(p.second) << 1);
    }
};

[[nodiscard]] std::pair<Point *, Point *> OrderedPointPair(Point *a, Point *b) noexcept
{
    const auto ua = reinterpret_cast<std::uintptr_t>(a);
    const auto ub = reinterpret_cast<std::uintptr_t>(b);
    return (ua <= ub) ? std::pair<Point *, Point *>(a, b) : std::pair<Point *, Point *>(b, a);
}

/// STL / tessellated stack-parallel caps often keep one loop per facet; `RebuildHoleCalibTopology` inner-loop
/// scan then stays empty and hole sidewalls never witness rims. Group caps by offset along build, trace the
/// boundary of the triangle/quad soup, and treat every non-outer loop (by |area|) as a hole rim. Nested solid
/// islands inside a cavity are a known false-positive edge case.
void AppendTessellatedStackParallelCapHoleInnerEdges(const Scene &scene, const glm::dvec3 &dirUnit,
                                                      std::unordered_set<const Edge *> &holeInnerEdgesOut)
{
    const double bucket = std::max(GeometryExperiments::kMergeCoplanarPlaneTolFloor, 1e-12);

    struct CapGroupKey
    {
        std::int64_t planeQ = 0;
        std::int8_t normalSign = 0;
        bool operator==(const CapGroupKey &o) const noexcept
        {
            return planeQ == o.planeQ && normalSign == o.normalSign;
        }
    };
    struct CapGroupKeyHash
    {
        std::size_t operator()(CapGroupKey k) const noexcept
        {
            return std::hash<std::int64_t>{}(k.planeQ) ^
                   (static_cast<std::size_t>(static_cast<unsigned>(k.normalSign + 9)) << 24);
        }
    };

    std::unordered_map<CapGroupKey, std::vector<const Face *>, CapGroupKeyHash> groups;
    groups.reserve(64);

    auto addFaceIfCap = [&](const Face *f)
    {
        if (f == nullptr || !f->HasGeometry() || !f->GetSurface().IsPlanar())
            return;
        if (!FaceCapParallelBuildDir(f, dirUnit))
            return;
        if (f->loops.empty())
            return;
        const glm::dvec3 p0 = f->loops[0][0].GetStartPosition();
        const std::int64_t planeQ =
            static_cast<std::int64_t>(std::llround(glm::dot(p0, dirUnit) / bucket));
        glm::dvec3 n = glm::normalize(f->GetSurface().GetNormal());
        if (!std::isfinite(n.x) || glm::length(n) < 1e-12)
            return;
        const std::int8_t normalSign =
            static_cast<std::int8_t>((glm::dot(n, dirUnit) >= 0.0) ? 1 : -1);
        groups[CapGroupKey{planeQ, normalSign}].push_back(f);
    };

    for (const Solid &solid : scene.solids)
        for (const Face *f : solid.faces)
            addFaceIfCap(f);
    for (const Face &f : scene.faces)
        addFaceIfCap(&f);

    auto computeSignedLoopArea = [](const glm::dvec3 &planeNormal, const std::vector<Edge *> &loop) -> double
    {
        if (loop.empty())
            return 0.0;
        Point *current = loop[0]->startPoint;
        if (current == nullptr)
            return 0.0;
        if (loop.size() > 1)
        {
            Edge *e0 = loop[0];
            Edge *e1 = loop[1];
            if (e0->endPoint == e1->startPoint || e0->endPoint == e1->endPoint)
                current = e0->startPoint;
            else
                current = e0->endPoint;
        }
        std::vector<glm::dvec3> positions;
        positions.reserve(loop.size() + 1);
        for (Edge *e : loop)
        {
            if (current == nullptr)
                return 0.0;
            positions.push_back(current->position);
            current = (e->startPoint == current) ? e->endPoint : e->startPoint;
        }
        double area = 0.0;
        for (size_t j = 0; j < positions.size(); ++j)
        {
            const size_t k = (j + 1) % positions.size();
            area += glm::dot(planeNormal, glm::cross(positions[j], positions[k]));
        }
        return area;
    };

    for (auto &kv : groups)
    {
        const std::vector<const Face *> &bucket = kv.second;
        std::vector<const Face *> tess;
        tess.reserve(bucket.size());
        for (const Face *f : bucket)
        {
            if (f != nullptr && f->loops.size() == 1)
                tess.push_back(f);
        }
        if (tess.size() < 2)
            continue;

        std::unordered_map<std::pair<Point *, Point *>, int, PointPairHash> edgeAdjacencyCount;
        std::unordered_map<std::pair<Point *, Point *>, Edge *, PointPairHash> edgeRepresentative;
        edgeAdjacencyCount.reserve(tess.size() * 4);
        edgeRepresentative.reserve(tess.size() * 4);

        for (const Face *f : tess)
        {
            for (const auto &oe : f->loops[0])
            {
                Edge *ed = oe.edge;
                if (ed == nullptr || ed->startPoint == nullptr || ed->endPoint == nullptr)
                    continue;
                const auto pr = OrderedPointPair(ed->startPoint, ed->endPoint);
                ++edgeAdjacencyCount[pr];
                edgeRepresentative[pr] = ed;
            }
        }

        std::vector<Edge *> boundaryEdges;
        for (const auto &ac : edgeAdjacencyCount)
        {
            if (ac.second != 1)
                continue;
            Edge *rep = edgeRepresentative[ac.first];
            if (rep != nullptr)
                boundaryEdges.push_back(rep);
        }
        if (boundaryEdges.size() < 3)
            continue;

        std::unordered_map<Point *, std::vector<Edge *>> adj;
        for (Edge *e : boundaryEdges)
        {
            if (e->startPoint == nullptr || e->endPoint == nullptr)
                continue;
            adj[e->startPoint].push_back(e);
            adj[e->endPoint].push_back(e);
        }

        std::unordered_set<Edge *> used;
        std::vector<std::vector<Edge *>> edgeLoops;

        for (Edge *startEdge : boundaryEdges)
        {
            if (used.count(startEdge))
                continue;

            std::vector<Edge *> loop;
            used.insert(startEdge);
            loop.push_back(startEdge);
            Point *next = startEdge->endPoint;
            bool hasUnused = false;
            if (next != nullptr)
            {
                for (Edge *e : adj[next])
                {
                    if (!used.count(e))
                    {
                        hasUnused = true;
                        break;
                    }
                }
            }
            if (!hasUnused)
                next = startEdge->startPoint;

            while (next != nullptr)
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
            edgeLoops.push_back(std::move(loop));
        }

        if (used.size() != boundaryEdges.size())
            continue;
        if (edgeLoops.size() <= 1)
            continue;

        const glm::dvec3 planeNormal = glm::normalize(tess[0]->GetSurface().GetNormal());
        if (!std::isfinite(planeNormal.x) || glm::length(planeNormal) < 1e-12)
            continue;

        std::sort(edgeLoops.begin(), edgeLoops.end(),
                  [&](const std::vector<Edge *> &a, const std::vector<Edge *> &b)
                  {
                      return std::abs(computeSignedLoopArea(planeNormal, a)) >
                             std::abs(computeSignedLoopArea(planeNormal, b));
                  });

        for (size_t li = 1; li < edgeLoops.size(); ++li)
        {
            for (Edge *e : edgeLoops[li])
            {
                if (e != nullptr)
                    holeInnerEdgesOut.insert(e);
            }
        }
    }
}

} // namespace

bool FaceNormalPerpendicularToBuild(const Face *face, const glm::dvec3 &buildDirWorld)
{
    if (face == nullptr || !face->HasGeometry() || !face->GetSurface().IsPlanar())
        return false;

    glm::dvec3 n = glm::normalize(face->GetSurface().GetNormal());
    if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z) || glm::length(n) < 1e-12)
        return false;

    const glm::dvec3 d = NormalizeBuildDir(buildDirWorld);
    return std::abs(glm::dot(n, d)) <= kWallNormalMaxAbsDotBuild;
}

void RebuildHoleCalibTopology(const Scene &scene, const glm::dvec3 &buildDirWorld,
                              std::unordered_set<const Edge *> &holeInnerEdgesOut)
{
    const glm::dvec3 d = NormalizeBuildDir(buildDirWorld);
    holeInnerEdgesOut.clear();

    using HoleProbe = GeometryExperiments::CalibHoleQualifyProbe;
    const HoleProbe probe = GeometryExperiments::kCalibHoleQualifyProbe;
    const bool scanInnerLoops =
        (probe == HoleProbe::All || probe == HoleProbe::MultiLoopCapOnly);
    const bool traceTessellated =
        (probe == HoleProbe::All || probe == HoleProbe::TessellatedRimCapOnly);

    auto scanFace = [&](const Face *f)
    {
        if (f == nullptr || !f->HasGeometry() || f->loops.size() < 2 || !f->GetSurface().IsPlanar())
            return;
        // Only caps ∥ build: inner loops on slanted pockets are not “holes in the layer” for slicing;
        // including them falsely tagged oblique cavity walls as `Hole` (hole radius offset).
        if (!FaceCapParallelBuildDir(f, d))
            return;
        for (size_t li = 1; li < f->loops.size(); ++li)
        {
            for (const auto &oe : f->loops[li])
            {
                if (oe.edge != nullptr)
                    holeInnerEdgesOut.insert(oe.edge);
            }
        }
    };

    if (scanInnerLoops)
    {
        for (const Solid &solid : scene.solids)
        {
            for (const Face *f : solid.faces)
                scanFace(f);
        }
        for (const Face &f : scene.faces)
            scanFace(&f);
    }

    if (traceTessellated)
        AppendTessellatedStackParallelCapHoleInnerEdges(scene, d, holeInnerEdgesOut);
}

bool FaceIsLayerCapParallelBuild(const Face *face, const glm::dvec3 &buildDirWorld)
{
    return FaceCapParallelBuildDir(face, NormalizeBuildDir(buildDirWorld));
}

bool FaceInFirstLayerSlab(const Face *face, const Scene *scene, double layerHeightMm,
                          const glm::dvec3 &buildDirWorld)
{
    if (face == nullptr || scene == nullptr || layerHeightMm <= 0.0)
        return false;

    const glm::dvec3 d = NormalizeBuildDir(buildDirWorld);
    const double sceneMin = SceneMinAlongBuildDir(*scene, d);
    if (sceneMin == std::numeric_limits<double>::max())
        return false;

    const double top = sceneMin + layerHeightMm;
    constexpr double kEps = 1e-5;

    double fMin = std::numeric_limits<double>::max();
    double fMax = std::numeric_limits<double>::lowest();
    for (const auto &loop : face->loops)
    {
        for (const auto &oe : loop)
        {
            const glm::dvec3 p0 = oe.GetStartPosition();
            const glm::dvec3 p1 = oe.GetEndPosition();
            const double t0 = glm::dot(p0, d);
            const double t1 = glm::dot(p1, d);
            fMin = std::min(fMin, std::min(t0, t1));
            fMax = std::max(fMax, std::max(t0, t1));
        }
    }

    return fMin >= sceneMin - kEps && fMax <= top + kEps;
}

bool FaceQualifiesAsHole(const Face *face, const glm::dvec3 &buildDirWorld,
                         const std::unordered_set<const Edge *> &layerHoleInnerEdges)
{
    (void)buildDirWorld;
    if (face == nullptr || layerHoleInnerEdges.empty())
        return false;

    for (const auto &loop : face->loops)
    {
        for (const auto &oe : loop)
        {
            if (oe.edge != nullptr && layerHoleInnerEdges.count(oe.edge))
                return true;
        }
    }
    return false;
}

CalibWorkflow ClassifyFace(const Face *face, const Scene *scene, double layerHeightMm,
                           const glm::dvec3 &buildDirWorld,
                           const std::unordered_set<const Edge *> &layerHoleInnerEdges)
{
    if (face == nullptr || scene == nullptr)
        return CalibWorkflow::None;

    (void)layerHeightMm;
    if (FaceQualifiesAsHole(face, buildDirWorld, layerHoleInnerEdges))
        return CalibWorkflow::Hole;
    return CalibWorkflow::Contour;
}

CalibWorkflow CombinePickedFaces(const Face *a, const Face *b, const Scene *scene, double layerHeightMm,
                                 const glm::dvec3 &buildDirWorld,
                                 const std::unordered_set<const Edge *> &layerHoleInnerEdges)
{
    if (a == nullptr && b == nullptr)
        return CalibWorkflow::None;
    if (b == nullptr)
        return ClassifyFace(a, scene, layerHeightMm, buildDirWorld, layerHoleInnerEdges);
    if (a == nullptr)
        return ClassifyFace(b, scene, layerHeightMm, buildDirWorld, layerHoleInnerEdges);

    const CalibWorkflow ca = ClassifyFace(a, scene, layerHeightMm, buildDirWorld, layerHoleInnerEdges);
    const CalibWorkflow cb = ClassifyFace(b, scene, layerHeightMm, buildDirWorld, layerHoleInnerEdges);

    if (!CalibSecondPickWorkflowsCompatible(ca, cb))
        return CalibWorkflow::None;

    if (ca == CalibWorkflow::Hole || cb == CalibWorkflow::Hole)
        return CalibWorkflow::Hole;
    if (ca == CalibWorkflow::Contour || cb == CalibWorkflow::Contour)
        return CalibWorkflow::Contour;
    return CalibWorkflow::None;
}

} // namespace CalibrateDistance
