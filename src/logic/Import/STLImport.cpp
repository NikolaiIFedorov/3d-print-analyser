#include "STLImport.hpp"
#include "GeometryExperiments.hpp"
#include "GeometryValidity.hpp"
#include "utils/log.hpp"
#include "scene/scene.hpp"

#include <RWStl.hxx>
#include <Poly_Triangulation.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <chrono>

bool STLImport::Import(
    const std::string &filePath,
    Scene *scene,
    STLImportStats *stats,
    const ImportProgressCallback *progress)
{
    using Clock = std::chrono::steady_clock;
    const Clock::time_point tStart = Clock::now();

    ReportImportProgress(progress, "Opening STL file...", 0.0f);
    LOG_DESC("Importing STL: " + filePath)

    Handle(Poly_Triangulation) mesh = RWStl::ReadFile(filePath.c_str());
    if (mesh.IsNull())
        return LOG_FALSE("Failed to read STL file: " + filePath);

    double parseMs = std::chrono::duration<double, std::milli>(Clock::now() - tStart).count();
    const Clock::time_point tBuildStart = Clock::now();

    ReportImportProgress(progress, "Building B-Rep from mesh...", 0.3f);
    
    BRepBuilderAPI_Sewing sewer(1e-4);
    for (int i = 1; i <= mesh->NbTriangles(); ++i)
    {
        int n1, n2, n3;
        mesh->Triangle(i).Get(n1, n2, n3);
        BRepBuilderAPI_MakePolygon poly(mesh->Node(n1), mesh->Node(n2), mesh->Node(n3), Standard_True);
        if (poly.IsDone())
        {
            BRepBuilderAPI_MakeFace mkFace(poly.Wire());
            if (mkFace.IsDone())
                sewer.Add(mkFace.Face());
        }
    }

    ReportImportProgress(progress, "Sewing faces...", 0.6f);
    sewer.Perform();
    TopoDS_Shape sewedShape = sewer.SewedShape();

    ReportImportProgress(progress, "Merging coplanar faces...", 0.8f);
    ShapeUpgrade_UnifySameDomain unifier(sewedShape, Standard_True, Standard_True, Standard_True);
    unifier.Build();
    TopoDS_Shape unifiedShape = unifier.Shape();

    double mergeMs = std::chrono::duration<double, std::milli>(Clock::now() - tBuildStart).count();

    ReportImportProgress(progress, "Populating scene...", 0.9f);
    Solid *solid = scene->CreateSolid({});
    scene->PopulateSolidFromOcctShape(solid, unifiedShape);

    if (stats != nullptr)
    {
        stats->isBinary = false; // RWStl doesn't expose this easily, default to false
        stats->triangleCount = mesh->NbTriangles();
        stats->uniquePoints = mesh->NbNodes();
        stats->faces = solid->faces.size();
        stats->parseMs = parseMs;
        stats->mergeMs = mergeMs;
        stats->totalMs = parseMs + mergeMs;
    }

    ReportImportProgress(progress, "STL Import Complete", 1.0f);
    return true;
}
