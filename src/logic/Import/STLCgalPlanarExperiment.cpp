#include "STLCgalPlanarExperiment.hpp"
#include "GeometryExperiments.hpp"
#include "scene/scene.hpp"

#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Polygon_mesh_processing/orient_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>
#include <CGAL/Polygon_mesh_processing/remesh_planar_patches.h>
#include <CGAL/Polygon_mesh_processing/repair.h>
#include <CGAL/Polygon_mesh_processing/repair_polygon_soup.h>

#include <boost/graph/graph_traits.hpp>

#include <map>
#include <unordered_map>
#include <vector>

#include "utils/log.hpp"

using K = CGAL::Exact_predicates_inexact_constructions_kernel;
namespace PMP = CGAL::Polygon_mesh_processing;
using CgalMesh = CGAL::Surface_mesh<K::Point_3>;
using HalfedgeDesc = boost::graph_traits<CgalMesh>::halfedge_descriptor;

namespace STLCgalPlanarExperiment
{
namespace
{
static void DetachFacesFromSolid(Scene &, Solid &solid)
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
        if (f->loops.size() != 1 || f->loops[0].size() != 3)
        {
            LOG_WARN("CGAL path: skipping non-triangular loop (expected STL triangle soup)");
            return false;
        }

        std::vector<std::size_t> tri(3);
        for (std::size_t i = 0; i < 3; ++i)
        {
            Point *pq = f->loops[0][i].GetStart();
            if (pq == nullptr)
            {
                LOG_WARN("CGAL path: triangle corner point null");
                return false;
            }
            tri[i] = indexOfPoint(pq);
        }
        trianglesOut.push_back(std::move(tri));
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
                LOG_WARN("CGAL path: CGAL vertex missing from remap");
                return false;
            }
            ring.push_back(itVp->second);
            h = CGAL::next(h, cgMesh);
        } while (h != h0);

        const std::size_t n = ring.size();
        if (n < 3)
        {
            LOG_WARN("CGAL path: degenerate facet after remesh");
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
                LOG_WARN("CGAL path: collapsed edge in facet");
                return false;
            }
            Edge *ed = scene->CreateEdge(a, b);
            if (ed == nullptr)
            {
                LOG_WARN("CGAL path: CreateEdge failed");
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

} // namespace

bool TryRemeshPlanarPatchesReplacingSolidFaces(Scene *scene, Solid *solid, MergeCoplanarDiagnostics *diagOut)
{
    (void)diagOut; // merge diagnostics are recorded by `Scene::MergeCoplanarFaces` after rebuild
    if (scene == nullptr || solid == nullptr)
        return false;

    std::vector<K::Point_3> coords;
    std::vector<std::vector<std::size_t>> tris;

    const std::size_t facesBefore = solid->faces.size();

    if (!BuildTriangleSoupFromSolid(*solid, coords, tris))
    {
        LOG_WARN("CGAL path: triangle soup build failed");
        return false;
    }

    PMP::orient_polygon_soup(coords, tris);

    CgalMesh tm;
    // Guard against CGAL precondition violations on hostile soups.
    if (!PMP::is_polygon_soup_a_polygon_mesh(tris))
    {
        PMP::repair_polygon_soup(coords, tris);
        if (!PMP::is_polygon_soup_a_polygon_mesh(tris))
        {
            LOG_WARN("CGAL path: invalid triangle soup (is_polygon_soup_a_polygon_mesh failed)");
            return false;
        }
    }
    PMP::polygon_soup_to_polygon_mesh(coords, tris, tm);
    if (tm.number_of_faces() == 0)
    {
        LOG_WARN("CGAL path: polygon_soup_to_polygon_mesh produced empty mesh");
        return false;
    }

    PMP::duplicate_non_manifold_vertices(tm);

    const K::FT cosTol(1.0 - GeometryExperiments::kMergeCoplanarNormalDotSlack);

    CgalMesh out;
    CGAL::Polygon_mesh_processing::remesh_planar_patches(tm, out,
                                                         CGAL::parameters::cosine_of_maximum_angle(cosTol));

    if (out.number_of_faces() == 0)
    {
        LOG_WARN("CGAL path: remesh_planar_patches produced empty mesh (non-manifold input?)");
        return false;
    }

    DetachFacesFromSolid(*scene, *solid);

    if (!RebuildSolidFromCgalMesh(scene, *solid, out))
    {
        LOG_WARN("CGAL path: rebuild Scene from CGAL mesh failed");
        return false;
    }

    const std::size_t facesAfter = solid->faces.size();

    LOG_DESC("CGAL remesh_planar_patches STL experiment: faces",
             std::to_string(facesBefore), "->", std::to_string(facesAfter));

    return true;
}

} // namespace STLCgalPlanarExperiment
