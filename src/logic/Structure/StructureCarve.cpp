#include "Structure/StructureCarve.hpp"

#include "GeometryValidity.hpp"
#include "Structure/StructureTriangulation.hpp"
#include "scene/scene.hpp"
#include "Geometry/OrientedEdge.hpp"
#include "Geometry/Surface.hpp"
#include "utils/log.hpp"

#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <exception>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace StructureCarve
{
namespace
{

static void DetachFacesFromSolid(Solid &solid)
{
    GeometryValidity::InvalidateSolidAppGeometryValidityCache(solid);
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

static TopoDS_Shape BuildVerticalPrismOcct(const std::vector<glm::dvec3> &footprintCCW, double zBottom, double zTop)
{
    const std::size_t n = footprintCCW.size();
    if (n < 3 || !(zBottom < zTop))
        return TopoDS_Shape();

    BRepBuilderAPI_MakePolygon poly;
    for (std::size_t i = 0; i < n; ++i)
    {
        const glm::dvec3 &p = footprintCCW[i];
        poly.Add(gp_Pnt(p.x, p.y, zBottom));
    }
    poly.Close();

    if (!poly.IsDone())
        return TopoDS_Shape();

    TopoDS_Face face = BRepBuilderAPI_MakeFace(poly.Wire());
    if (face.IsNull())
        return TopoDS_Shape();

    BRepPrimAPI_MakePrism prismMaker(face, gp_Vec(0, 0, zTop - zBottom));
    return prismMaker.Shape();
}

} // namespace

bool TryApplyStructureCarve(Scene *scene,
                            Solid *solid,
                            const std::vector<const Face *> &faces,
                            const StructureTriangulation::BakeParams &params,
                            std::string *errOut,
                            const std::function<bool()> *shouldAbort,
                            const std::function<void(const std::string &)> *workerTrace)
{
    auto fail = [&](const char *msg) -> bool
    {
        if (errOut != nullptr)
            *errOut = msg;
        return false;
    };

    auto aborted = [&]() -> bool
    {
        return shouldAbort != nullptr && (*shouldAbort)();
    };

    auto invokeTrace = [&](const std::string &phase)
    {
        if (workerTrace != nullptr && *workerTrace)
            (*workerTrace)(phase);
    };

    if (scene == nullptr || solid == nullptr || faces.empty())
        return true;

    if (solid->occtShape.IsNull())
        return fail("Solid has no OCCT shape.");

    try
    {
        invokeTrace("enter");
        if (aborted())
            return fail("Structure carve cancelled.");

        TopoDS_Shape currentShape = solid->occtShape;

        double zMinWorld = 0.0;
        double zMaxWorld = 0.0;
        SolidWorldZBounds(*solid, zMinWorld, zMaxWorld);
        constexpr double kZMarginMm = 0.5;
        const double zBottom = zMinWorld - kZMarginMm;
        const double zTop = zMaxWorld + kZMarginMm;

        invokeTrace("after_z_bounds");

        bool anyPrismApplied = false;
        for (const Face *face : faces)
        {
            if (aborted())
                return fail("Structure carve cancelled.");
            if (face == nullptr || face->dependency != solid)
                continue;
            if (face->surface == nullptr || !face->surface->IsPlanar())
            {
                Log::Background("Structure carve: skipping non-planar face");
                continue;
            }
            const glm::dvec3 n = face->surface->GetNormal();
            const double nz = glm::length(n) > 1e-12 ? std::abs(n.z / glm::length(n)) : 0.0;
            if (nz < 0.995)
            {
                Log::Background("Structure carve: skipping face — |n·z| < 0.995 (prism extruder is world Z)");
                continue;
            }

            if (aborted())
                return fail("Structure carve cancelled.");
            invokeTrace("before_footprint");
#if defined(CAD_USE_CGAL)
            const std::vector<std::vector<glm::dvec3>> rings =
                StructureTriangulation::BuildCarveFootprintOuterRingsWorld(face, params, workerTrace);
#else
            const std::vector<std::vector<glm::dvec3>> rings;
#endif
            invokeTrace(std::string("footprint_done_rings_") + std::to_string(rings.size()));
            if (aborted())
                return fail("Structure carve cancelled.");
            for (const std::vector<glm::dvec3> &ring : rings)
            {
                if (aborted())
                    return fail("Structure carve cancelled.");
                if (ring.size() < 3)
                {
                    invokeTrace("ring_skip_short");
                    continue;
                }
                invokeTrace("before_prism");
                TopoDS_Shape prism = BuildVerticalPrismOcct(ring, zBottom, zTop);
                invokeTrace("after_prism");
                if (prism.IsNull())
                {
                    invokeTrace("ring_skip_invalid_prism");
                    continue;
                }

                if (aborted())
                    return fail("Structure carve cancelled.");
                invokeTrace("before_boolean");
                BRepAlgoAPI_Cut cutter(currentShape, prism);
                cutter.Build();
                if (cutter.HasErrors())
                    return fail("OCCT boolean difference failed.");
                invokeTrace("after_boolean");
                currentShape = cutter.Shape();
                anyPrismApplied = true;
            }
        }

        if (!anyPrismApplied)
            return fail("No carve prisms were applied (rings empty, faces skipped, or prisms invalid).");

        if (aborted())
            return fail("Structure carve cancelled.");
        invokeTrace("before_detach");
        DetachFacesFromSolid(*solid);
        
        scene->PopulateSolidFromOcctShape(solid, currentShape);

        invokeTrace("after_rebuild");
        scene->MergeCoplanarFaces(solid, nullptr, nullptr);
        Log::Background("Structure carve: applied to solid with " + std::to_string(faces.size()) + " face pick(s)");
        return true;
    }
    catch (const std::exception &e)
    {
        return fail((std::string("Structure carve exception: ") + e.what()).c_str());
    }
    catch (...)
    {
        return fail("Structure carve unknown exception.");
    }
}

} // namespace StructureCarve
