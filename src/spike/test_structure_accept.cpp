#include "Analysis/Analysis.hpp"
#include "Analysis/AnalysisTypes.hpp"
#include "Calibrate/CalibDistanceType.hpp"
#include "Import/STEPImport.hpp"
#include "Structure/StructureCarve.hpp"
#include "Structure/StructureTriangulation.hpp"
#include "scene/scene.hpp"
#include "display/rendering/SceneRenderer/Patch/patch.hpp"
#include "display/rendering/SceneRenderer/Wireframe/Wireframe.hpp"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <iostream>
#include <limits>
#include <queue>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

static void LogPreviewChordStats(const std::vector<std::pair<glm::vec3, glm::vec3>> &preview,
                                 const char *label)
{
    if (preview.empty())
    {
        std::cout << label << " previewSegs=0\n";
        return;
    }
    std::vector<double> lens;
    lens.reserve(preview.size());
    for (const auto &s : preview)
        lens.push_back(static_cast<double>(glm::length(s.second - s.first)));
    std::sort(lens.begin(), lens.end());
    const double median = lens[lens.size() / 2];
    const double maxLen = lens.back();
    int jumpCount = 0;
    for (double l : lens)
    {
        if (l > std::max(8.0, median * 6.0))
            ++jumpCount;
    }
    std::cout << label << " previewSegs=" << preview.size() << " medianChordMm=" << median
              << " maxChordMm=" << maxLen << " jumpChords=" << jumpCount << "\n";
}

static Face *FindUpwardTopFace(Solid *solid)
{
    Face *best = nullptr;
    for (Face *f : solid->faces)
    {
        if (f == nullptr || !f->HasGeometry() || !f->surface->IsPlanar())
            continue;
        const glm::dvec3 n = f->surface->GetNormal();
        if (n.z > 0.995)
            best = f;
    }
    return best;
}

/// Mirrors Display post-analysis UI paths that previously dereferenced null edges.
static bool SimulatePostAcceptUiPaths(Scene *scene, AnalysisResults &results, const char *label)
{
    std::unordered_set<const Face *> overhangFaces;
    for (const auto &[solid, flaws] : results.faceFlawRanges)
    {
        for (const auto &ff : flaws)
        {
            if (ff.flaw == FaceFlawKind::OVERHANG && ff.face != nullptr && ff.face->HasGeometry())
                overhangFaces.insert(ff.face);
        }
    }

    std::unordered_set<const Face *> visited;
    for (const Face *seed : overhangFaces)
    {
        if (visited.count(seed))
            continue;
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
                        if (overhangFaces.count(neighbor) && !visited.count(neighbor))
                        {
                            visited.insert(neighbor);
                            bfs.push(neighbor);
                        }
                    }
                }
            }
        }
    }

    glm::dvec3 modelMin(std::numeric_limits<double>::max());
    glm::dvec3 modelMax(std::numeric_limits<double>::lowest());
    for (const Face &face : scene->faces)
    {
        if (!face.HasGeometry())
            continue;
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

    size_t tombstones = 0;
    for (const Face &face : scene->faces)
    {
        if (!face.HasGeometry())
            ++tombstones;
    }
    std::cout << label << " ui-path ok tombstones=" << tombstones << " scene_faces=" << scene->faces.size()
              << "\n";
    return true;
}

static bool RunCarveAnalysisRender(Scene *scene, Solid *solid, Face *top, const char *label)
{
    StructureTriangulation::BakeParams params;
    params.insetMm = 2.0;
    params.chordTolMm = 0.02;
    params.minFeatureMm = 1.5;

    std::string err;
    if (!StructureCarve::TryApplyStructureCarve(scene, solid, {top}, params, &err))
    {
        std::cerr << label << " carve failed: " << err << "\n";
        return false;
    }

    AnalysisResults results = Analysis::Instance().AnalyzeScene(scene);
    PruneDefunctAnalysisResults(results, scene);

    AnalysisResults stale;
    stale.faceFlawRanges[solid] = {FaceFlaw{top, FaceFlawKind::OVERHANG, ZBounds{0.0, 0.0}, {}}};
    PruneDefunctAnalysisResults(stale, scene);
    if (!stale.faceFlawRanges[solid].empty())
    {
        std::cerr << label << " stale prune failed\n";
        return false;
    }

    if (!SimulatePostAcceptUiPaths(scene, results, label))
        return false;

    // Tool-switch clears calibrate picks and may rebuild hole topology on the carved scene.
    std::unordered_set<const Edge *> holeInnerEdges;
    CalibrateDistance::RebuildHoleCalibTopology(*scene, CalibrateDistance::DefaultCalibrateBuildDirection(),
                                                holeInnerEdges);

    Patch patch;
    Wireframe wire;
    std::vector<Vertex> vertsPlain;
    std::vector<uint32_t> idxPlain;
    std::vector<Vertex> vertsTint;
    std::vector<uint32_t> idxTint;
    int viewport[4] = {0, 0, 800, 600};
    patch.Generate(scene, vertsPlain, idxPlain, viewport, nullptr);
    patch.Generate(scene, vertsTint, idxTint, viewport, &results);
    wire.Generate(scene, vertsPlain, idxPlain, nullptr);
    wire.Generate(scene, vertsTint, idxTint, &results);
    if (vertsPlain.size() != vertsTint.size() || idxPlain.size() != idxTint.size())
    {
        std::cerr << label << " recolor layout mismatch plain=" << vertsPlain.size()
                  << " tint=" << vertsTint.size() << "\n";
        return false;
    }
    std::cout << label << " ok solid_faces=" << solid->faces.size() << " verts=" << vertsPlain.size() << "\n";
    return true;
}

static bool RunStagingClonePath(Scene *scene, Solid *solid, Face *top, const char *label)
{
    std::unordered_map<const Face *, Face *> remap;
    std::unique_ptr<Scene> staging = scene->Clone(&remap);
    if (!staging)
    {
        std::cerr << label << " clone failed\n";
        return false;
    }
    Face *stagingTop = nullptr;
    if (top != nullptr)
    {
        auto it = remap.find(top);
        if (it != remap.end())
            stagingTop = it->second;
    }
    if (stagingTop == nullptr && !staging->solids.empty())
        stagingTop = FindUpwardTopFace(&staging->solids[0]);
    if (stagingTop == nullptr)
    {
        std::cerr << label << " no staging top face\n";
        return false;
    }
    Solid *stagingSolid = stagingTop->dependency;
    if (!RunCarveAnalysisRender(staging.get(), stagingSolid, stagingTop, label))
        return false;
    // Simulate Accept: drop original backup; carved staging is canonical.
    return true;
}

static Face *FindUpwardTopFaceWithMostLoops(Solid *solid)
{
    Face *best = nullptr;
    size_t bestLoops = 0;
    for (Face *f : solid->faces)
    {
        if (f == nullptr || !f->HasGeometry() || !f->surface->IsPlanar())
            continue;
        const glm::dvec3 n = f->surface->GetNormal();
        if (n.z <= 0.995)
            continue;
        if (f->loops.size() >= bestLoops)
        {
            bestLoops = f->loops.size();
            best = f;
        }
    }
    return best;
}

static Face *FindUpwardSlantedFace(Solid *solid)
{
    Face *best = nullptr;
    double bestSlant = 0.0;
    for (Face *f : solid->faces)
    {
        if (f == nullptr || !f->HasGeometry() || !f->surface->IsPlanar())
            continue;
        const glm::dvec3 n = f->surface->GetNormal();
        const double nLen = glm::length(n);
        if (nLen < 1e-12)
            continue;
        const double nz = n.z / nLen;
        if (nz < StructureTriangulation::kMinUpComponent || nz > 0.995)
            continue;
        const double slant = 1.0 - nz;
        if (slant > bestSlant)
        {
            bestSlant = slant;
            best = f;
        }
    }
    return best;
}

static bool RunSlantedTopWedge()
{
    BRepBuilderAPI_MakePolygon profile;
    profile.Add(gp_Pnt(0.0, 0.0, 0.0));
    profile.Add(gp_Pnt(40.0, 0.0, 0.0));
    profile.Add(gp_Pnt(40.0, 0.0, 8.0));
    profile.Add(gp_Pnt(0.0, 0.0, 28.0));
    profile.Close();
    if (!profile.IsDone())
    {
        std::cerr << "slanted-wedge: profile failed\n";
        return false;
    }

    BRepBuilderAPI_MakeFace mkFace(profile.Wire());
    if (!mkFace.IsDone())
    {
        std::cerr << "slanted-wedge: face failed\n";
        return false;
    }

    const TopoDS_Shape wedge = BRepPrimAPI_MakePrism(mkFace.Face(), gp_Vec(0.0, 35.0, 0.0)).Shape();

    Scene sceneDirect;
    Solid *solidDirect = sceneDirect.CreateSolid({}, false);
    sceneDirect.PopulateSolidFromOcctShape(solidDirect, wedge);
    Face *slantDirect = FindUpwardSlantedFace(solidDirect);
    if (slantDirect == nullptr)
    {
        std::cerr << "slanted-wedge: no eligible slanted face\n";
        return false;
    }
    if (!RunCarveAnalysisRender(&sceneDirect, solidDirect, slantDirect, "slanted-wedge-direct"))
        return false;

    Scene sceneStaging;
    Solid *solidStaging = sceneStaging.CreateSolid({}, false);
    sceneStaging.PopulateSolidFromOcctShape(solidStaging, wedge);
    Face *slantStaging = FindUpwardSlantedFace(solidStaging);
    if (slantStaging == nullptr)
    {
        std::cerr << "slanted-wedge-staging: no slanted face\n";
        return false;
    }
    if (!RunStagingClonePath(&sceneStaging, solidStaging, slantStaging, "slanted-wedge-staging"))
        return false;
    return true;
}

static bool RunHoledTopBox()
{
    TopoDS_Shape box = BRepPrimAPI_MakeBox(30.0, 30.0, 20.0).Shape();

    gp_Trsf cylXform;
    cylXform.SetTranslation(gp_Vec(10.0, 10.0, -2.0));
    TopoDS_Shape cyl = BRepBuilderAPI_Transform(BRepPrimAPI_MakeCylinder(4.0, 24.0).Shape(), cylXform).Shape();

    gp_Trsf boxCutXform;
    boxCutXform.SetTranslation(gp_Vec(22.0, 22.0, -2.0));
    TopoDS_Shape rectCut =
        BRepBuilderAPI_Transform(BRepPrimAPI_MakeBox(6.0, 6.0, 24.0).Shape(), boxCutXform).Shape();

    TopoDS_Shape cut1 = BRepAlgoAPI_Cut(box, cyl).Shape();
    TopoDS_Shape holedBox = BRepAlgoAPI_Cut(cut1, rectCut).Shape();

    Scene sceneDirect;
    Solid *solidDirect = sceneDirect.CreateSolid({}, false);
    sceneDirect.PopulateSolidFromOcctShape(solidDirect, holedBox);
    Face *topDirect = FindUpwardTopFaceWithMostLoops(solidDirect);
    if (topDirect == nullptr || topDirect->loops.size() < 2)
    {
        std::cerr << "holed-box: expected upward top face with holes, loops="
                  << (topDirect ? topDirect->loops.size() : 0) << "\n";
        return false;
    }
    if (!RunCarveAnalysisRender(&sceneDirect, solidDirect, topDirect, "holed-box-direct"))
        return false;

    Scene sceneStaging;
    Solid *solidStaging = sceneStaging.CreateSolid({}, false);
    sceneStaging.PopulateSolidFromOcctShape(solidStaging, holedBox);
    Face *topStaging = FindUpwardTopFaceWithMostLoops(solidStaging);
    if (topStaging == nullptr)
    {
        std::cerr << "holed-box-staging: no top face\n";
        return false;
    }
    if (!RunStagingClonePath(&sceneStaging, solidStaging, topStaging, "holed-box-staging"))
        return false;
    return true;
}

static bool IsEligibleStructureFace(const Face *face)
{
    if (face == nullptr || face->surface == nullptr || !face->surface->IsPlanar() || face->loops.empty())
        return false;
    if (face->surface->GetNormal().z < StructureTriangulation::kMinUpComponent)
        return false;
    return true;
}

static bool AssertStructure3Offset(Face *face)
{
    StructureTriangulation::BakeParams params;
    params.insetMm = 2.0;
    params.chordTolMm = 0.02;
    params.minFeatureMm = 1.5;

    const TopoDS_Shape footprint = StructureTriangulation::BuildOffsetFootprintOnFace(face, params);
    if (footprint.IsNull() || footprint.ShapeType() != TopAbs_FACE)
    {
        std::cerr << "structure3-offset: expected planar offset face\n";
        return false;
    }

    const auto rings =
        StructureTriangulation::ExtractOffsetRingsFromFootprint(footprint, params.chordTolMm, 0.0);
    if (rings.size() < 2)
    {
        std::cerr << "structure3-offset: expected at least 2 footprint rings, got " << rings.size() << "\n";
        return false;
    }

    const auto previewRings = StructureTriangulation::BuildTrimmedFacePlanePreviewRings(face, params);
    const auto rawRings = StructureTriangulation::BuildFacePlaneOffsetPreviewRings(face, params);
    if (previewRings.empty())
    {
        std::cerr << "structure3-offset: preview rings empty\n";
        return false;
    }
    if (previewRings.size() < 2 && rawRings.size() < 2)
    {
        std::cerr << "structure3-offset: expected preview (outer + holes), got " << previewRings.size()
                  << " rings\n";
        return false;
    }
    if (rawRings.size() < face->loops.size())
    {
        std::cerr << "structure3-offset: raw preview missing hole loop(s): rawRings=" << rawRings.size()
                  << " sourceLoops=" << face->loops.size() << "\n";
        return false;
    }

    const auto previewLines = StructureTriangulation::BuildCutOutlinePreviewLines(face, params);
    if (previewLines.empty())
    {
        std::cerr << "structure3-offset: preview lines empty\n";
        return false;
    }
    if (previewLines.size() < 40)
    {
        std::cerr << "structure3-offset: preview lines incomplete: segs=" << previewLines.size() << "\n";
        return false;
    }

    std::cout << "structure3-offset ok footprintRings=" << rings.size()
              << " previewRings=" << previewRings.size() << " rawRings=" << rawRings.size()
              << " previewSegs=" << previewLines.size() << "\n";
    return true;
}

static bool RunStructure3OffsetOnly()
{
    Scene scene;
    Solid *solid = ImportSTEP(&scene, "/Users/nikolnotai/Documents/Structure3.stp");
    if (solid == nullptr)
    {
        std::cerr << "structure3-offset: import failed\n";
        return false;
    }

    Face *face = nullptr;
    for (Face *f : solid->faces)
    {
        if (f == nullptr || !f->HasGeometry() || !f->surface->IsPlanar())
            continue;
        const double nz = f->surface->GetNormal().z;
        if (nz >= StructureTriangulation::kMinUpComponent && nz < 0.995 && f->loops.size() > 1)
            face = f;
    }
    if (face == nullptr)
    {
        std::cerr << "structure3-offset: no eligible slanted holed face\n";
        return false;
    }

    int eligibleCount = 0;
    for (Face *f : solid->faces)
    {
        if (IsEligibleStructureFace(f))
            ++eligibleCount;
    }
    std::cout << "structure3-offset eligibleFaces=" << eligibleCount << "\n";

    return AssertStructure3Offset(face);
}

static bool RunStructure3DoubleCarve()
{
    Scene scene;
    Solid *solid = ImportSTEP(&scene, "/Users/nikolnotai/Documents/Structure3.stp");
    if (solid == nullptr)
    {
        std::cerr << "structure3: import failed\n";
        return false;
    }

    auto findEligible = [](Solid *s) -> Face *
    {
        Face *slanted = nullptr;
        for (Face *f : s->faces)
        {
            if (f == nullptr || !f->HasGeometry() || !f->surface->IsPlanar())
                continue;
            const double nz = f->surface->GetNormal().z;
            if (nz >= StructureTriangulation::kMinUpComponent && nz < 0.995)
                slanted = f;
        }
        if (slanted != nullptr)
            return slanted;
        for (Face *f : s->faces)
        {
            if (f == nullptr || !f->HasGeometry() || !f->surface->IsPlanar())
                continue;
            if (f->surface->GetNormal().z >= StructureTriangulation::kMinUpComponent)
                return f;
        }
        return nullptr;
    };

    Face *face = findEligible(solid);
    if (face == nullptr)
    {
        std::cerr << "structure3: no eligible face\n";
        return false;
    }

    if (!AssertStructure3Offset(face))
        return false;

    StructureTriangulation::BakeParams params;
    params.insetMm = 2.0;
    params.chordTolMm = 0.02;
    params.minFeatureMm = 1.5;

    if (!RunCarveAnalysisRender(&scene, solid, face, "structure3-carve1"))
        return false;

    size_t downwardFaces = 0;
    for (const Face &f : scene.faces)
    {
        if (!f.HasGeometry() || f.surface == nullptr || !f.surface->IsPlanar())
            continue;
        if (f.surface->GetNormal().z < -0.5)
            ++downwardFaces;
    }
    if (downwardFaces == 0)
    {
        std::cerr << "structure3: expected a downward-facing cap after slanted carve\n";
        return false;
    }

    // Second carve on the same slanted cap typically has no inset footprint left; carve1 is the
    // regression guard for Structure3.stp.
    return true;
}

static bool RunSyntheticBox()
{
    Scene sceneDirect;
    Solid *solidDirect = sceneDirect.CreateSolid({}, false);
    TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    sceneDirect.PopulateSolidFromOcctShape(solidDirect, box);
    Face *topDirect = FindUpwardTopFace(solidDirect);
    if (topDirect == nullptr)
    {
        std::cerr << "synthetic: no top face\n";
        return false;
    }
    if (!RunCarveAnalysisRender(&sceneDirect, solidDirect, topDirect, "synthetic-direct"))
        return false;

    Scene sceneStaging;
    Solid *solidStaging = sceneStaging.CreateSolid({}, false);
    sceneStaging.PopulateSolidFromOcctShape(solidStaging, box);
    Face *topStaging = FindUpwardTopFace(solidStaging);
    if (topStaging == nullptr)
    {
        std::cerr << "synthetic: no staging top face\n";
        return false;
    }
    if (!RunStagingClonePath(&sceneStaging, solidStaging, topStaging, "synthetic-staging"))
        return false;
    return true;
}

static bool RunStepBox()
{
    Scene sceneDirect;
    Solid *solidDirect = ImportSTEP(&sceneDirect, "/Users/nikolnotai/Documents/Box.stp");
    if (solidDirect == nullptr)
    {
        std::cerr << "step: import failed\n";
        return false;
    }
    Face *topDirect = FindUpwardTopFace(solidDirect);
    if (topDirect == nullptr)
    {
        std::cerr << "step: no top face\n";
        return false;
    }
    if (!RunCarveAnalysisRender(&sceneDirect, solidDirect, topDirect, "step-direct"))
        return false;

    Scene sceneStaging;
    Solid *solidStaging = ImportSTEP(&sceneStaging, "/Users/nikolnotai/Documents/Box.stp");
    if (solidStaging == nullptr)
    {
        std::cerr << "step-staging: import failed\n";
        return false;
    }
    Face *topStaging = FindUpwardTopFace(solidStaging);
    if (topStaging == nullptr)
    {
        std::cerr << "step-staging: no top face\n";
        return false;
    }
    if (!RunStagingClonePath(&sceneStaging, solidStaging, topStaging, "step-staging"))
        return false;
    return true;
}

static Face *FindLargestPlanarFaceByLoopCount(Solid *solid)
{
    Face *best = nullptr;
    size_t bestLoops = 0;
    for (Face *f : solid->faces)
    {
        if (f == nullptr || !f->HasGeometry() || !f->surface->IsPlanar())
            continue;
        if (f->loops.size() >= bestLoops)
        {
            bestLoops = f->loops.size();
            best = f;
        }
    }
    return best;
}

static void DiagnoseStructureFile(const char *path, const char *label)
{
    Scene scene;
    Solid *solid = ImportSTEP(&scene, path);
    if (solid == nullptr)
    {
        std::cerr << label << ": import failed\n";
        return;
    }
    Face *face = FindUpwardTopFaceWithMostLoops(solid);
    if (face == nullptr)
        face = FindLargestPlanarFaceByLoopCount(solid);
    if (face == nullptr)
    {
        std::cerr << label << ": no planar face\n";
        return;
    }

    StructureTriangulation::BakeParams params;
    params.insetMm = 2.0;
    params.chordTolMm = 0.02;
    params.minFeatureMm = 1.5;

    const auto rawRings = StructureTriangulation::BuildFacePlaneOffsetPreviewRings(face, params);
    const auto trimmedRings = StructureTriangulation::BuildTrimmedFacePlanePreviewRings(face, params);
    const auto previewLines = StructureTriangulation::BuildCutOutlinePreviewLines(face, params);

    const glm::dvec3 n = face->surface != nullptr ? face->surface->GetNormal() : glm::dvec3(0, 0, 1);
    std::cout << label << " faceLoops=" << face->loops.size() << " normalZ=" << n.z
              << " rawRings=" << rawRings.size() << " trimmedRings=" << trimmedRings.size()
              << " previewSegs=" << previewLines.size() << "\n";
    for (size_t i = 0; i < trimmedRings.size(); ++i)
        std::cout << label << " trimmedRing" << i << " pts=" << trimmedRings[i].size() << "\n";
    for (size_t i = 0; i < rawRings.size(); ++i)
        std::cout << label << " rawRing" << i << " pts=" << rawRings[i].size() << "\n";
}

static bool RunStructureTest17TiltPreview()
{
    Scene scene;
    Solid *solid = ImportSTEP(&scene, "/Users/nikolnotai/Documents/StructureTest17tilt.stp");
    if (solid == nullptr)
    {
        std::cerr << "structuretest17-tilt: import failed\n";
        return false;
    }

    Face *face = FindUpwardSlantedFace(solid);
    if (face == nullptr)
        face = FindLargestPlanarFaceByLoopCount(solid);
    if (face == nullptr)
    {
        std::cerr << "structuretest17-tilt: no slanted planar face\n";
        return false;
    }

    StructureTriangulation::BakeParams params;
    params.insetMm = 2.0;
    params.chordTolMm = 0.02;
    params.minFeatureMm = 1.5;

    const auto trimmedRings = StructureTriangulation::BuildTrimmedFacePlanePreviewRings(face, params);
    if (trimmedRings.empty())
    {
        std::cerr << "structuretest17-tilt: trimmed rings empty\n";
        return false;
    }
    const auto previewLines = StructureTriangulation::BuildCutOutlinePreviewLines(face, params);
    if (previewLines.empty())
    {
        std::cerr << "structuretest17-tilt: preview lines empty\n";
        return false;
    }

    std::cout << "structuretest17-tilt ok trimmedRings=" << trimmedRings.size()
              << " previewSegs=" << previewLines.size() << "\n";
    return true;
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv[1][0] != '\0')
    {
        DiagnoseStructureFile(argv[1], "diag");
        return 0;
    }
    if (!RunSyntheticBox())
        return 1;
    if (!RunHoledTopBox())
        return 1;
    if (!RunSlantedTopWedge())
        return 1;
    if (!RunStructure3OffsetOnly())
        return 1;
    if (!RunStructure3DoubleCarve())
        return 1;
    if (!RunStructureTest17TiltPreview())
        return 1;
    if (!RunStepBox())
        return 1;
    std::cout << "ALL OK\n";
    return 0;
}
