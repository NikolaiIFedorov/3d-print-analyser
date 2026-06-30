// Repro spike for two reported Structure-tool bugs:
//  1) struts missing after changing the inset parameter (re-carve)
//  2) "two sets of top faces" after changing the inset parameter
// Mimics Display::LaunchStructureStagingCarveJob: every carve starts from a fresh
// clone of the same original scene; only params.insetMm varies.

#include "GeometryOps/Fillet2D.hpp"
#include "Import/STEPImport.hpp"
#include "Structure/StructureCarve.hpp"
#include "Structure/StructureTriangulation.hpp"
#include "scene/scene.hpp"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <glm/glm.hpp>

#include <iostream>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

static Face *FindUpwardTopFaceWithMostLoops(Solid *solid)
{
    Face *best = nullptr;
    size_t bestLoops = 0;
    for (Face *f : solid->faces)
    {
        if (f == nullptr || !f->HasGeometry() || !f->surface->IsPlanar())
            continue;
        if (f->surface->GetNormal().z <= 0.995)
            continue;
        if (f->loops.size() >= bestLoops)
        {
            bestLoops = f->loops.size();
            best = f;
        }
    }
    return best;
}

// Counts upward-facing planar faces in the solid, bucketed by representative Z (rounded to 0.01mm).
static void ReportTopFaces(Solid *solid, const char *label)
{
    std::map<long long, int> byZ;
    for (Face *f : solid->faces)
    {
        if (f == nullptr || !f->HasGeometry() || !f->surface->IsPlanar())
            continue;
        if (f->surface->GetNormal().z <= 0.9)
            continue;
        double z = 0.0;
        size_t n = 0;
        for (const auto &loop : f->loops)
            for (const auto &oe : loop)
                if (oe.GetStart() != nullptr)
                {
                    z += oe.GetStartPosition().z;
                    ++n;
                }
        if (n == 0)
            continue;
        z /= static_cast<double>(n);
        byZ[static_cast<long long>(std::llround(z * 100.0))]++;
    }
    std::cout << label << " upward-face Z buckets: ";
    for (const auto &[zKey, count] : byZ)
        std::cout << "z=" << (zKey / 100.0) << " count=" << count << "  ";
    std::cout << "\n";
}

// Mirrors Display::IsStructureFaceEligible's geometric core (planar, upward, big enough).
static bool EligibleForCarve(const Face *f)
{
    if (f == nullptr || !f->HasGeometry() || f->surface == nullptr || !f->surface->IsPlanar() ||
        f->loops.empty())
        return false;
    const glm::dvec3 n = f->surface->GetNormal();
    const double len = glm::length(n);
    if (len < 1e-12 || n.z / len < StructureTriangulation::kMinUpComponent)
        return false;
    glm::dvec3 mn(1e300), mx(-1e300);
    for (const auto &loop : f->loops)
        for (const auto &oe : loop)
            if (oe.GetStart() != nullptr)
            {
                mn = glm::min(mn, oe.GetStartPosition());
                mx = glm::max(mx, oe.GetStartPosition());
            }
    const glm::dvec3 span = mx - mn;
    return std::max({span.x, span.y, span.z}) >= StructureTriangulation::kMinFaceSpanMm &&
           std::max(span.x, span.y) >= StructureTriangulation::kMinFaceSpanMm;
}

static void CarveAtInset(Scene &original, double inset)
{
    std::cout << "\n===== inset " << inset << " =====\n";
    StructureTriangulation::ClearBakeCache();

    std::unordered_map<const Face *, Face *> remap;
    std::unique_ptr<Scene> staging = original.Clone(&remap);
    if (!staging || staging->solids.empty())
    {
        std::cerr << "clone failed\n";
        return;
    }

    StructureTriangulation::BakeParams params;
    params.insetMm = inset;
    params.chordTolMm = 0.02;
    params.minFeatureMm = 1.5;

    // Like Display::LaunchStructureStagingCarveJob: group eligible faces per solid.
    std::unordered_map<Solid *, std::vector<const Face *>> bySolid;
    for (Face &f : staging->faces)
    {
        if (f.dependency == nullptr || !EligibleForCarve(&f))
            continue;
        bySolid[f.dependency].push_back(&f);
    }
    std::cout << "eligible faces: ";
    for (auto &[s, fs] : bySolid)
        std::cout << fs.size() << " ";
    std::cout << "\n";

    size_t totalStruts = 0;
    for (auto &[s, fs] : bySolid)
        for (const Face *f : fs)
            totalStruts += StructureTriangulation::BuildStrutPreviewLines(f, params).size();
    std::cout << "strut centerlines (all faces): " << totalStruts << "\n";

    // Probe the curved carve pipeline step by step, mirroring BuildVerticalPrismOcct.
    for (auto &[s, fs] : bySolid)
    {
        int fi = 0;
        for (const Face *f : fs)
        {
            TopoDS_Shape fp = StructureTriangulation::BuildCurvedCarveFootprintMvp(f, params);
            std::cout << "face[" << fi << "] curvedFootprint null=" << fp.IsNull() << "\n";
            if (!fp.IsNull())
            {
                TopoDS_Shape notched =
                    StructureTriangulation::NotchStrutsIntoCurvedFootprint(f, params, fp);
                std::cout << "face[" << fi << "] notched same-as-input=" << (notched.IsSame(fp) ? 1 : 0)
                          << "\n";
                TopoDS_Shape filleted =
                    GeometryOps::FilletCurvedFootprint(notched, params.insetMm);
                std::cout << "face[" << fi << "] filleted same-as-notched="
                          << (filleted.IsSame(notched) ? 1 : 0) << "\n";
                TopoDS_Shape prism =
                    StructureTriangulation::ExtrudeCurvedFootprintForCarve(filleted, -10.0, 30.0);
                std::cout << "face[" << fi << "] curvedPrism null=" << prism.IsNull() << "\n";
                if (prism.IsNull())
                {
                    BRepCheck_Analyzer fpCheck(filleted, Standard_True);
                    std::cout << "face[" << fi << "] filleted-footprint valid=" << fpCheck.IsValid()
                              << "\n";
                    BRepPrimAPI_MakePrism rawPrism(filleted, gp_Vec(0.0, 0.0, 40.0));
                    if (!rawPrism.Shape().IsNull())
                    {
                        BRepCheck_Analyzer przCheck(rawPrism.Shape(), Standard_True);
                        std::cout << "face[" << fi << "] raw prism valid=" << przCheck.IsValid()
                                  << "\n";
                        int badFaces = 0, badWires = 0, badEdges = 0;
                        for (TopExp_Explorer e(rawPrism.Shape(), TopAbs_FACE); e.More(); e.Next())
                            if (!przCheck.IsValid(e.Current()))
                                ++badFaces;
                        for (TopExp_Explorer e(rawPrism.Shape(), TopAbs_WIRE); e.More(); e.Next())
                            if (!przCheck.IsValid(e.Current()))
                                ++badWires;
                        for (TopExp_Explorer e(rawPrism.Shape(), TopAbs_EDGE); e.More(); e.Next())
                            if (!przCheck.IsValid(e.Current()))
                                ++badEdges;
                        std::cout << "face[" << fi << "] raw prism badFaces=" << badFaces
                                  << " badWires=" << badWires << " badEdges=" << badEdges << "\n";
                    }
                }
            }
            ++fi;
        }
    }

    for (auto &[s, fs] : bySolid)
    {
        std::string err;
        const bool ok = StructureCarve::TryApplyStructureCarve(staging.get(), s, fs, params, &err);
        std::cout << "carve ok=" << ok << (err.empty() ? "" : (" err=" + err))
                  << " solid_faces=" << s->faces.size() << "\n";
        if (ok)
            ReportTopFaces(s, "after-carve");
    }
}

int main(int argc, char **argv)
{
    Scene original;
    if (argc > 1)
    {
        Solid *imported = ImportSTEP(&original, argv[1]);
        if (imported == nullptr)
        {
            std::cerr << "import failed: " << argv[1] << "\n";
            return 1;
        }
        std::cout << "imported " << argv[1] << " solids=" << original.solids.size()
                  << " faces=" << original.faces.size() << "\n";
    }
    else
    {
        TopoDS_Shape box = BRepPrimAPI_MakeBox(30.0, 30.0, 20.0).Shape();

        gp_Trsf cylXform;
        cylXform.SetTranslation(gp_Vec(10.0, 10.0, -2.0));
        TopoDS_Shape cyl =
            BRepBuilderAPI_Transform(BRepPrimAPI_MakeCylinder(4.0, 24.0).Shape(), cylXform).Shape();

        gp_Trsf boxCutXform;
        boxCutXform.SetTranslation(gp_Vec(22.0, 22.0, -2.0));
        TopoDS_Shape rectCut =
            BRepBuilderAPI_Transform(BRepPrimAPI_MakeBox(6.0, 6.0, 24.0).Shape(), boxCutXform).Shape();

        TopoDS_Shape cut1 = BRepAlgoAPI_Cut(box, cyl).Shape();
        TopoDS_Shape holedBox = BRepAlgoAPI_Cut(cut1, rectCut).Shape();

        Solid *origSolid = original.CreateSolid({}, false);
        original.PopulateSolidFromOcctShape(origSolid, holedBox);
        if (FindUpwardTopFaceWithMostLoops(origSolid) == nullptr)
        {
            std::cerr << "setup failed: no top face\n";
            return 1;
        }
    }

    const double insets[] = {2.0, 2.5, 1.5, 3.0, 1.0};
    for (double inset : insets)
        CarveAtInset(original, inset);
    return 0;
}
