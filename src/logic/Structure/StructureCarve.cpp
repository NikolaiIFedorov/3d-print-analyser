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
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BOPAlgo_BuilderFace.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <TopExp_Explorer.hxx>
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

static TopoDS_Shape BuildVerticalPrismOcct(const Face *face, double zBottom, double zTop, const StructureTriangulation::BakeParams &params)
{
    if (face == nullptr || face->loops.empty() || !(zBottom < zTop))
        return TopoDS_Shape();

    std::unique_ptr<BRepBuilderAPI_MakeFace> mkFace;
    for (const auto &loop : face->loops)
    {
        BRepBuilderAPI_MakePolygon poly;
        for (const OrientedEdge &oe : loop)
        {
            if (oe.GetStart() != nullptr)
            {
                glm::dvec3 p = oe.GetStartPosition();
                poly.Add(gp_Pnt(p.x, p.y, zBottom));
            }
        }
        poly.Close();
        if (poly.IsDone())
        {
            if (!mkFace)
            {
                mkFace = std::make_unique<BRepBuilderAPI_MakeFace>(poly.Wire());
            }
            else
            {
                TopoDS_Wire w = poly.Wire();
                w.Reverse();
                mkFace->Add(w);
            }
        }
    }

    if (!mkFace || !mkFace->IsDone())
        return TopoDS_Shape();

    TopoDS_Face occtFace = mkFace->Face();

    if (params.insetMm <= 1e-6)
    {
        BRepPrimAPI_MakePrism prismMaker(occtFace, gp_Vec(0, 0, zTop - zBottom));
        return prismMaker.Shape();
    }

    // Step 1: Inset by 2 * insetMm
    BRepOffsetAPI_MakeOffset offsetMaker;
    offsetMaker.Init(occtFace, GeomAbs_Arc);
    offsetMaker.Perform(-2.0 * params.insetMm);
    if (!offsetMaker.IsDone())
        return TopoDS_Shape();
    TopoDS_Shape innerShape = offsetMaker.Shape();

    TopTools_ListOfShape innerEdges;
    for (TopExp_Explorer ex(innerShape, TopAbs_EDGE); ex.More(); ex.Next()) {
        innerEdges.Append(ex.Current());
    }
    if (innerEdges.IsEmpty())
        return TopoDS_Shape();

    BOPAlgo_BuilderFace innerBuilder;
    innerBuilder.SetFace(occtFace);
    innerBuilder.SetShapes(innerEdges);
    innerBuilder.Perform();
    const TopTools_ListOfShape& innerFaces = innerBuilder.Areas();

    // Step 2: Outset by insetMm
    TopoDS_Shape currentUnion;
    bool firstUnion = true;
    for (TopTools_ListIteratorOfListOfShape it(innerFaces); it.More(); it.Next()) {
        TopoDS_Face f = TopoDS::Face(it.Value());
        BRepOffsetAPI_MakeOffset offsetMaker2;
        offsetMaker2.Init(f, GeomAbs_Arc);
        offsetMaker2.Perform(params.insetMm);
        if (!offsetMaker2.IsDone()) continue;
        TopoDS_Shape outset = offsetMaker2.Shape();
        
        TopTools_ListOfShape outsetEdges;
        for (TopExp_Explorer ex(outset, TopAbs_EDGE); ex.More(); ex.Next()) {
            outsetEdges.Append(ex.Current());
        }
        if (outsetEdges.IsEmpty()) continue;

        BOPAlgo_BuilderFace outsetBuilder;
        outsetBuilder.SetFace(occtFace);
        outsetBuilder.SetShapes(outsetEdges);
        outsetBuilder.Perform();
        
        TopoDS_Shape outsetFace;
        if (outsetBuilder.Areas().Extent() > 0) {
            outsetFace = outsetBuilder.Areas().First();
        } else {
            continue;
        }
        
        if (firstUnion) {
            currentUnion = outsetFace;
            firstUnion = false;
        } else {
            BRepAlgoAPI_Fuse fuser(currentUnion, outsetFace);
            fuser.Build();
            if (fuser.IsDone()) {
                currentUnion = fuser.Shape();
            }
        }
    }

    if (firstUnion) return TopoDS_Shape();

    ShapeUpgrade_UnifySameDomain unifier(currentUnion, Standard_True, Standard_True, Standard_True);
    unifier.Build();
    currentUnion = unifier.Shape();

    // Step 3: Extrude the footprint
    TopoDS_Shape prism;
    bool firstPrism = true;
    for (TopExp_Explorer ex(currentUnion, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        BRepPrimAPI_MakePrism prismMaker(f, gp_Vec(0, 0, zTop - zBottom));
        if (firstPrism) {
            prism = prismMaker.Shape();
            firstPrism = false;
        } else {
            BRepAlgoAPI_Fuse fuser(prism, prismMaker.Shape());
            fuser.Build();
            if (fuser.IsDone()) {
                prism = fuser.Shape();
            }
        }
    }

    return prism;
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
            invokeTrace("before_prism");
            TopoDS_Shape prism = BuildVerticalPrismOcct(face, zBottom, zTop, params);
            invokeTrace("after_prism");
            if (prism.IsNull())
            {
                invokeTrace("prism_fail");
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
