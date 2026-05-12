#include "Structure/StructureCarve.hpp"

#include "Structure/StructureTriangulation.hpp"
#include "scene/scene.hpp"
#include "Geometry/OrientedEdge.hpp"
#include "Geometry/Surface.hpp"
#include "utils/log.hpp"

#if defined(CAD_USE_CGAL)

#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Polygon_mesh_processing/corefinement.h>
#include <CGAL/Polygon_mesh_processing/orient_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>
#include <CGAL/Polygon_mesh_processing/repair.h>
#include <boost/graph/graph_traits.hpp>

#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <unordered_map>
#include <vector>

using K = CGAL::Exact_predicates_inexact_constructions_kernel;
namespace PMP = CGAL::Polygon_mesh_processing;
using CgalMesh = CGAL::Surface_mesh<K::Point_3>;
using HalfedgeDesc = boost::graph_traits<CgalMesh>::halfedge_descriptor;

namespace StructureCarve
{
namespace
{

static void DetachFacesFromSolid(Solid &solid)
{
    std::vector<Face *> copy = solid.faces;
    solid.faces.clear();
    for (Face *pf : copy)
    {
        if (pf == nullptr)
            continue;
        for (auto &loop : pf->loops)
        {
            for (auto &oe : loop)
                if (oe.edge != nullptr)
                    oe.edge->dependencies.erase(pf);
        }
        pf->loops.clear();
        pf->dependency = nullptr;
    }
}

static bool BuildTriangleSoupFromSolid(Solid &solid,
                                       std::vector<K::Point_3> &coords,
                                       std::vector<std::vector<std::size_t>> &trianglesOut)
{
    coords.clear();
    trianglesOut.clear();
    std::unordered_map<Point *, std::size_t> pointIndex;

    auto indexOfPoint = [&](Point *p) -> std::size_t
    {
        auto it = pointIndex.find(p);
        if (it != pointIndex.end())
            return it->second;
        glm::dvec3 g = p->position;
        coords.push_back(K::Point_3(g.x, g.y, g.z));
        const std::size_t ix = coords.size() - 1;
        pointIndex.emplace(p, ix);
        return ix;
    };

    for (Face *f : solid.faces)
    {
        if (f == nullptr || f->dependency != &solid || f->loops.empty())
            continue;
        if (f->loops.size() != 1)
        {
            LOG_WARN("Structure carve: multi-loop faces not supported");
            return false;
        }
        const auto &loop = f->loops[0];
        const std::size_t n = loop.size();
        if (n < 3)
            continue;

        std::vector<std::size_t> idx(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            Point *pq = loop[i].GetStart();
            if (pq == nullptr)
            {
                LOG_WARN("Structure carve: face loop vertex null");
                return false;
            }
            idx[i] = indexOfPoint(pq);
        }

        if (n == 3)
        {
            trianglesOut.push_back({idx[0], idx[1], idx[2]});
        }
        else
        {
            // Fan from vertex 0 — correct for convex planar facets (typical merged STL quads).
            for (std::size_t i = 1; i + 1 < n; ++i)
                trianglesOut.push_back({idx[0], idx[i], idx[i + 1]});
        }
    }

    return !trianglesOut.empty() && coords.size() >= 3;
}

static bool RebuildSolidFromCgalMesh(Scene *scene, Solid &solid, const CgalMesh &cgMesh)
{
    std::map<CgalMesh::Vertex_index, Point *> vxToPt;

    for (CgalMesh::Vertex_index vd : CGAL::vertices(cgMesh))
    {
        const K::Point_3 qp = cgMesh.point(vd);
        Point *const p =
            scene->CreatePoint(glm::dvec3(CGAL::to_double(qp.x()), CGAL::to_double(qp.y()), CGAL::to_double(qp.z())));
        vxToPt.emplace(vd, p);
    }

    for (CgalMesh::Face_index fid : CGAL::faces(cgMesh))
    {
        HalfedgeDesc h = CGAL::halfedge(fid, cgMesh);
        HalfedgeDesc h0 = h;
        std::vector<Point *> ring;
        do
        {
            CgalMesh::Vertex_index vi = CGAL::target(h, cgMesh);
            auto itVp = vxToPt.find(vi);
            if (itVp == vxToPt.end())
            {
                LOG_WARN("Structure carve: CGAL vertex missing from remap");
                return false;
            }
            ring.push_back(itVp->second);
            h = CGAL::next(h, cgMesh);
        } while (h != h0);

        const std::size_t n = ring.size();
        if (n < 3)
        {
            LOG_WARN("Structure carve: degenerate facet after boolean");
            return false;
        }

        std::vector<Edge *> boundary;
        boundary.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            Point *a = ring[i];
            Point *b = ring[(i + 1) % n];
            if (a == b)
            {
                LOG_WARN("Structure carve: collapsed edge in facet");
                return false;
            }
            Edge *ed = scene->CreateEdge(a, b);
            if (ed == nullptr)
            {
                LOG_WARN("Structure carve: CreateEdge failed");
                return false;
            }
            boundary.push_back(ed);
        }

        Face *nf = scene->CreateFace({boundary});
        nf->dependency = &solid;
        solid.faces.push_back(nf);
    }

    return !solid.faces.empty();
}

static void SolidWorldZBounds(const Solid &solid, double &zMin, double &zMax)
{
    zMin = std::numeric_limits<double>::infinity();
    zMax = -std::numeric_limits<double>::infinity();
    for (const Face *f : solid.faces)
    {
        if (f == nullptr)
            continue;
        for (const auto &loop : f->loops)
        {
            for (const OrientedEdge &oe : loop)
            {
                Point *p = oe.GetStart();
                if (p == nullptr)
                    continue;
                zMin = std::min(zMin, p->position.z);
                zMax = std::max(zMax, p->position.z);
            }
        }
    }
    if (!(zMin <= zMax))
    {
        zMin = 0.0;
        zMax = 0.0;
    }
}

static CgalMesh BuildVerticalPrismMesh(const std::vector<glm::dvec3> &footprintCCW, double zBottom,
                                       double zTop)
{
    CgalMesh mesh;
    const std::size_t n = footprintCCW.size();
    if (n < 3 || !(zBottom < zTop))
        return mesh;

    std::vector<K::Point_3> coords;
    std::vector<std::vector<std::size_t>> faces;
    coords.reserve(n * 2);
    faces.reserve(2 * n + 2 * (n - 2));

    auto addPoint = [&](const glm::dvec3 &p) -> std::size_t
    {
        coords.emplace_back(p.x, p.y, p.z);
        return coords.size() - 1;
    };

    std::vector<std::size_t> bi;
    std::vector<std::size_t> ti;
    bi.reserve(n);
    ti.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const glm::dvec3 &p = footprintCCW[i];
        bi.push_back(addPoint(glm::dvec3(p.x, p.y, zBottom)));
    }
    for (std::size_t i = 0; i < n; ++i)
    {
        const glm::dvec3 &p = footprintCCW[i];
        ti.push_back(addPoint(glm::dvec3(p.x, p.y, zTop)));
    }

    for (std::size_t i = 1; i + 1 < n; ++i)
        faces.push_back({bi[0], bi[i + 1], bi[i]});
    for (std::size_t i = 1; i + 1 < n; ++i)
        faces.push_back({ti[0], ti[i], ti[i + 1]});

    for (std::size_t i = 0; i < n; ++i)
    {
        const std::size_t ni = (i + 1) % n;
        faces.push_back({bi[i], bi[ni], ti[ni]});
        faces.push_back({bi[i], ti[ni], ti[i]});
    }

    PMP::orient_polygon_soup(coords, faces);
    PMP::polygon_soup_to_polygon_mesh(coords, faces, mesh);
    PMP::duplicate_non_manifold_vertices(mesh);
    return mesh;
}

} // namespace

bool TryApplyStructureCarve(Scene *scene,
                            Solid *solid,
                            const std::vector<const Face *> &faces,
                            const StructureTriangulation::BakeParams &params,
                            std::string *errOut)
{
    auto fail = [&](const char *msg) -> bool
    {
        if (errOut != nullptr)
            *errOut = msg;
        return false;
    };

    if (scene == nullptr || solid == nullptr || faces.empty())
        return true;

    try
    {
        std::vector<K::Point_3> coords;
        std::vector<std::vector<std::size_t>> tris;
        if (!BuildTriangleSoupFromSolid(*solid, coords, tris))
            return fail("Could not build triangle soup from solid.");

        PMP::orient_polygon_soup(coords, tris);
        CgalMesh tm;
        PMP::polygon_soup_to_polygon_mesh(coords, tris, tm);
        if (tm.number_of_faces() == 0)
            return fail("Empty CGAL mesh from solid.");
        PMP::duplicate_non_manifold_vertices(tm);

        double zMinWorld = 0.0;
        double zMaxWorld = 0.0;
        SolidWorldZBounds(*solid, zMinWorld, zMaxWorld);
        constexpr double kZMarginMm = 0.5;
        const double zBottom = zMinWorld - kZMarginMm;
        const double zTop = zMaxWorld + kZMarginMm;

        bool anyPrismApplied = false;
        for (const Face *face : faces)
        {
            if (face == nullptr || face->dependency != solid)
                continue;
            if (face->surface == nullptr || !face->surface->IsPlanar())
            {
                LOG_WARN("Structure carve: skipping non-planar face");
                continue;
            }
            const glm::dvec3 n = face->surface->GetNormal();
            const double nz = glm::length(n) > 1e-12 ? std::abs(n.z / glm::length(n)) : 0.0;
            if (nz < 0.995)
            {
                LOG_WARN("Structure carve: skipping face — |n·z| < 0.995 (prism extruder is world Z)");
                continue;
            }

            const std::vector<std::vector<glm::dvec3>> rings =
                StructureTriangulation::BuildCarveFootprintOuterRingsWorld(face, params);
            for (const std::vector<glm::dvec3> &ring : rings)
            {
                if (ring.size() < 3)
                    continue;
                CgalMesh prism = BuildVerticalPrismMesh(ring, zBottom, zTop);
                if (!prism.is_valid() || prism.number_of_faces() == 0)
                    continue;

                CgalMesh diffOut;
                if (!PMP::corefine_and_compute_difference(tm, prism, diffOut))
                    return fail("CGAL boolean difference failed (try smaller inset or check mesh).");
                tm = std::move(diffOut);
                anyPrismApplied = true;
            }
        }

        if (!anyPrismApplied)
            return fail("No carve prisms were applied (rings empty, faces skipped, or prisms invalid).");

        DetachFacesFromSolid(*solid);
        if (!RebuildSolidFromCgalMesh(scene, *solid, tm))
            return fail("Failed to rebuild solid from carved mesh.");

        scene->MergeCoplanarFaces(solid, nullptr, nullptr);
        LOG_DESC("Structure carve: applied to solid with", std::to_string(faces.size()), "face pick(s)");
        return true;
    }
    catch (const std::exception &ex)
    {
        if (errOut != nullptr)
            *errOut = std::string("CGAL exception: ") + ex.what();
        return false;
    }
    catch (...)
    {
        if (errOut != nullptr)
            *errOut = "CGAL exception (unknown type).";
        return false;
    }
}

} // namespace StructureCarve

#endif // CAD_USE_CGAL
