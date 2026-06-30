#include "Structure/StructureCarve.hpp"

#include "GeometryOps/Fillet2D.hpp"
#include "GeometryOps/OcctProgressRelay.hpp"
#include "GeometryValidity.hpp"
#include "Structure/StructureTriangulation.hpp"
#include "scene/scene.hpp"
#include "Geometry/OrientedEdge.hpp"
#include "Geometry/Surface.hpp"
#include "utils/log.hpp"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools_ReShape.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>

#include <glm/glm.hpp>

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
        pf->ClearForRemoval();
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

// --- Prism builder -----------------------------------------------------------

/// Builds the carve footprint from the exact same notched-and-filleted outline the preview shows
/// (`BuildNotchedCarveFootprint` shares its ring pipeline with `BuildCutOutlinePreviewLines` and
/// `BuildStrutRailPreviewLines`), so the carved result always matches what the user previewed —
/// struts squared off via the same boolean-clip cut, corners rounded to the same `insetMm` radius.
static TopoDS_Shape BuildVerticalPrismOcct(const Face *face, double zBottom, double zTop,
                                           const StructureTriangulation::BakeParams &params)
{
    if (face == nullptr || face->loops.empty() || !(zBottom < zTop))
        return TopoDS_Shape();

    // Curve-native carve (default): build the inset/outset offset footprint with analytic edges
    // intact, project tilted faces to the cutting plane with an affine shear, notch struts and round
    // corners (ChFi2d) on the curved geometry, then extrude — so a circular hole carves a cylinder
    // wall, not a ~450-face polygon prism. Any failure falls through to the polygon path below, so
    // the curve path is strictly self-healing.
    if (StructureTriangulation::StructureCurvedCarveEnabled())
    {
        static const bool kSkipNotch = []()
        {
            const char *e = std::getenv("CAD_STRUCTURE_SKIP_NOTCH");
            return e != nullptr && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
        }();
        static const bool kSkipFillet = []()
        {
            const char *e = std::getenv("CAD_STRUCTURE_SKIP_FILLET");
            return e != nullptr && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
        }();

        TopoDS_Shape curvedFootprint = StructureTriangulation::BuildCurvedCarveFootprintMvp(face, params);
        if (!curvedFootprint.IsNull())
        {
            if (!kSkipNotch)
                curvedFootprint = StructureTriangulation::NotchStrutsIntoCurvedFootprint(face, params, curvedFootprint);
            if (!kSkipFillet)
                curvedFootprint = GeometryOps::FilletCurvedFootprint(curvedFootprint, params.insetMm);
            const TopoDS_Shape curvedPrism =
                StructureTriangulation::ExtrudeCurvedFootprintForCarve(curvedFootprint, zBottom, zTop);
            if (!curvedPrism.IsNull())
                return curvedPrism;
        }
        // else: fall through to the polygon path below.
    }

    TopoDS_Shape footprint = StructureTriangulation::BuildNotchedCarveFootprint(face, params);

    glm::dvec3 normalHint{0.0, 0.0, 1.0};
    if (face->surface != nullptr)
        normalHint = face->surface->GetNormal();
    return StructureTriangulation::ExtrudeFlattenedFootprint(footprint, zBottom, zTop, params.chordTolMm,
                                                             params.minFeatureMm, normalHint);
}

} // namespace

bool TryApplyStructureCarve(Scene *scene,
                            Solid *solid,
                            const std::vector<const Face *> &faces,
                            const StructureTriangulation::BakeParams &params,
                            std::string *errOut,
                            const std::function<bool()> *shouldAbort,
                            const std::function<void(const std::string &)> *workerTrace,
                            const ImportProgressCallback *progress,
                            const std::vector<const Face *> &keepFaces)
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

    // One stable label for the whole call: the progress bar widget sizes its track to the label's
    // rendered text width (UIRenderer's underline-style progress strip), so swapping labels on
    // every checkpoint — even though the float only ever increases — visually reads as the bar
    // resetting each time the text width changes. Keep the text constant; only the float moves.
    static const std::string kCarveProgressLabel = "Carving structure...";
    auto reportProgress = [&](float progress01)
    {
        ReportImportProgress(progress, kCarveProgressLabel, progress01);
    };

    if (scene == nullptr || solid == nullptr || faces.empty())
        return true;

    if (solid->occtShape.IsNull())
        return fail("Solid has no OCCT shape.");

    try
    {
        invokeTrace("enter");
        reportProgress(0.0f);
        if (aborted())
            return fail("Structure carve cancelled.");

        TopoDS_Shape currentShape = solid->occtShape;

        double zMinWorld = 0.0;
        double zMaxWorld = 0.0;
        SolidWorldZBounds(*solid, zMinWorld, zMaxWorld);
        constexpr double kZMarginMm = 0.5;
        const double zTopSolid = zMaxWorld + kZMarginMm;
        const double zBottomSolid = zMinWorld - kZMarginMm;

        invokeTrace("after_z_bounds");

        static const bool kTopoTrace = []()
        {
            const char *e = std::getenv("CAD_STRUCTURE_TOPO_TRACE");
            return e != nullptr && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
        }();

        // Returns count of OCCT faces that would produce loops=0 in PopulateSolidFromOcctShape:
        // faces where every wire yields zero traversable edges via BRepTools_WireExplorer.
        // This catches the broken-wire case (wire exists in topology but can't be walked)
        // that TopExp_Explorer alone misses.
        auto countDegenerateFaces = [](const TopoDS_Shape &shape) -> int
        {
            int count = 0;
            for (TopExp_Explorer exF(shape, TopAbs_FACE); exF.More(); exF.Next())
            {
                const TopoDS_Face &occtFace = TopoDS::Face(exF.Current());
                bool hasAnyEdge = false;
                for (TopExp_Explorer exW(occtFace, TopAbs_WIRE); exW.More(); exW.Next())
                {
                    BRepTools_WireExplorer exE(TopoDS::Wire(exW.Current()));
                    if (exE.More())
                    {
                        hasAnyEdge = true;
                        break;
                    }
                }
                if (!hasAnyEdge)
                    ++count;
            }
            return count;
        };

        // Per-face prism build + boolean cut is the bulk of carve work; map it onto [0.05, 0.75]
        // so the bar advances steadily across faces instead of sitting frozen until the whole
        // solid finishes (the old per-solid-only step counter).
        constexpr float kFacesProgressStart = 0.05f;
        constexpr float kFacesProgressEnd = 0.75f;
        const size_t totalFaceSlots = faces.size();

        bool anyPrismApplied = false;
        int faceIdx = 0;
        size_t loopPos = 0;
        for (const Face *face : faces)
        {
            const size_t loopPosAtEntry = loopPos++;
            const float sliceStart = MapImportProgress(static_cast<float>(loopPosAtEntry) / totalFaceSlots,
                                                         kFacesProgressStart, kFacesProgressEnd);
            const float sliceEnd = MapImportProgress(static_cast<float>(loopPosAtEntry + 1) / totalFaceSlots,
                                                       kFacesProgressStart, kFacesProgressEnd);

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
            const double nLen = glm::length(n);
            const double nz = nLen > 1e-12 ? n.z / nLen : 0.0;
            if (nz < StructureTriangulation::kMinUpComponent)
            {
                Log::Background("Structure carve: skipping face — normal Z below minimum (XY projection too vertical)");
                continue;
            }

            const int degenerateBefore = kTopoTrace ? countDegenerateFaces(currentShape) : 0;

            // Tilted faces are projected to the horizontal XY cutting plane (BuildCurvedCarveFootprintMvp's
            // affine flatten) and so are treated exactly like horizontal faces: cut the full vertical
            // span of the solid for a clean through-lattice. (Previously tilted faces special-cased
            // zBottom to faceZMin - margin, which stopped the prism short and left a floor in the holes.)
            const double zBottom = zBottomSolid;
            (void)nz;

            if (aborted())
                return fail("Structure carve cancelled.");
            invokeTrace("before_prism");
            reportProgress(sliceStart);
            TopoDS_Shape prism = BuildVerticalPrismOcct(face, zBottom, zTopSolid, params);
            invokeTrace("after_prism");
            const float afterPrismProgress = MapImportProgress(0.3f, sliceStart, sliceEnd);
            reportProgress(afterPrismProgress);
            if (prism.IsNull())
            {
                invokeTrace("prism_fail");
                ++faceIdx;
                continue;
            }

            if (aborted())
                return fail("Structure carve cancelled.");
            invokeTrace("before_boolean");
            BRepAlgoAPI_Cut cutter(currentShape, prism);
            Handle(GeometryOps::OcctProgressRelay) cutRelay = new GeometryOps::OcctProgressRelay(
                [&](double localProgress01)
                {
                    reportProgress(MapImportProgress(static_cast<float>(localProgress01), afterPrismProgress, sliceEnd));
                });
            cutter.Build(cutRelay->Start());
            if (cutter.HasErrors())
                return fail("OCCT boolean difference failed.");
            invokeTrace("after_boolean");
            reportProgress(sliceEnd);
            currentShape = cutter.Shape();
            anyPrismApplied = true;

            // Strip degenerate faces (loops=0) the solid boolean may introduce when a notched
            // prism wall is nearly coincident with an existing solid face. These faces pass
            // BRepCheck_Analyzer but BRepTools_WireExplorer can't traverse their wire; they
            // have no traversable boundary edges and contribute nothing to the solid boundary,
            // so removing them leaves the geometry intact.
            {
                BRepTools_ReShape reShape;
                bool hasDegenFace = false;
                for (TopExp_Explorer exF(currentShape, TopAbs_FACE); exF.More(); exF.Next())
                {
                    const TopoDS_Face &occtFace = TopoDS::Face(exF.Current());
                    bool hasAnyEdge = false;
                    for (TopExp_Explorer exW(occtFace, TopAbs_WIRE); exW.More(); exW.Next())
                    {
                        BRepTools_WireExplorer exE(TopoDS::Wire(exW.Current()));
                        if (exE.More()) { hasAnyEdge = true; break; }
                    }
                    if (!hasAnyEdge)
                    {
                        reShape.Remove(occtFace);
                        hasDegenFace = true;
                    }
                }
                if (hasDegenFace)
                    currentShape = reShape.Apply(currentShape);
            }

            if (kTopoTrace)
            {
                int totalFaces = 0;
                for (TopExp_Explorer ex(currentShape, TopAbs_FACE); ex.More(); ex.Next())
                    ++totalFaces;
                const int degenerateAfter = countDegenerateFaces(currentShape);
                const glm::dvec3 faceN = face->surface->GetNormal();
                std::cerr << "[topo-trace] cut face_idx=" << faceIdx
                          << " normal=(" << faceN.x << "," << faceN.y << "," << faceN.z << ")"
                          << " inset=" << params.insetMm
                          << " total_faces=" << totalFaces
                          << " degenerate=" << degenerateAfter
                          << " new_degenerate=" << (degenerateAfter - degenerateBefore)
                          << std::endl;
            }

            ++faceIdx;
        }

        if (!anyPrismApplied)
            return fail("No carve prisms were applied (rings empty, faces skipped, or prisms invalid).");

        if (aborted())
            return fail("Structure carve cancelled.");

        // Each `BRepAlgoAPI_Cut` against a notched/strutted/filleted prism splits the solid's faces
        // into many same-surface fragments along the cut boundary (carving just two faces of a
        // 17-face solid produced 7000+ tiny coplanar `Face`s once rebuilt) — too many for the
        // renderer/picker to handle interactively. Unify them at the OCCT level, before rebuilding
        // `Face`/`Solid` topology, using the same tool STL/OBJ import already relies on to
        // consolidate fragmented meshed geometry (`ShapeUpgrade_UnifySameDomain` merges same-surface
        // faces and same-curve edges natively in OCCT — far faster than `Scene::MergeCoplanarFaces`'s
        // rescan-from-zero pairwise loop on the rebuilt topology, which chokes at this scale).
        invokeTrace("before_unify");
        reportProgress(kFacesProgressEnd);

        if (kTopoTrace)
        {
            int faceCountBefore = 0;
            double maxVtxTol = 0.0;
            for (TopExp_Explorer ex(currentShape, TopAbs_FACE); ex.More(); ex.Next())
                ++faceCountBefore;
            for (TopExp_Explorer ex(currentShape, TopAbs_VERTEX); ex.More(); ex.Next())
                maxVtxTol = std::max(maxVtxTol, BRep_Tool::Tolerance(TopoDS::Vertex(ex.Current())));
            std::cerr << "[topo-trace] pre-fix: faces=" << faceCountBefore
                      << " max_vertex_tol=" << maxVtxTol << std::endl;
        }

        ShapeUpgrade_UnifySameDomain unifier(currentShape, Standard_True, Standard_True, Standard_True);
        unifier.SetLinearTolerance(1e-4);
        unifier.SetAngularTolerance(1e-3);
        for (const Face *kf : keepFaces)
        {
            if (kf != nullptr && !kf->occtFace.IsNull())
                unifier.KeepShape(kf->occtFace);
        }
        unifier.Build();
        if (!unifier.Shape().IsNull())
            currentShape = unifier.Shape();

        if (kTopoTrace)
        {
            int faceCountAfter = 0;
            for (TopExp_Explorer ex(currentShape, TopAbs_FACE); ex.More(); ex.Next())
                ++faceCountAfter;
            std::cerr << "[topo-trace] post-unify: faces=" << faceCountAfter << std::endl;
        }

        invokeTrace("after_unify");
        reportProgress(0.85f);

        if (aborted())
            return fail("Structure carve cancelled.");
        invokeTrace("before_detach");
        DetachFacesFromSolid(*solid);
        reportProgress(0.9f);

        // Use a coarser mesh for the carved result: fillet radii equal insetMm, so a chord
        // tolerance of insetMm/6 gives ~12 segments per quarter-circle — visually smooth without
        // the 0.1mm import-quality density that produces thousands of triangles on cylindrical holes.
        const double strutDeflection = std::max(0.05, std::min(0.5, params.insetMm / 6.0));
        scene->PopulateSolidFromOcctShape(solid, currentShape, strutDeflection);

        invokeTrace("after_rebuild");
        reportProgress(1.0f);
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
