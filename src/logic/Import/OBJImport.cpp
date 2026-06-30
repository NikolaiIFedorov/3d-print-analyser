#include "OBJImport.hpp"
#include "GeometryOps/OcctProgressRelay.hpp"
#include "scene/scene.hpp"
#include "utils/log.hpp"

#include <RWObj.hxx>
#include <Poly_Triangulation.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <algorithm>
#include <chrono>

bool OBJImport::Import(const std::string &filePath, Scene *scene, const ImportProgressCallback *progress)
{
    using Clock = std::chrono::steady_clock;
    const Clock::time_point tStart = Clock::now();

    ReportImportProgress(progress, "Opening OBJ file...", 0.0f);
    LOG_DESC("Importing OBJ: " + filePath)

    Handle(Poly_Triangulation) mesh = RWObj::ReadFile(filePath.c_str());
    if (mesh.IsNull())
        return LOG_FALSE("Failed to read OBJ file: " + filePath);

    ReportImportProgress(progress, "Building B-Rep from mesh...", 0.3f);

    const int triangleCount = mesh->NbTriangles();
    const int triangleProgressStride = std::max(1, triangleCount / 200);
    BRepBuilderAPI_Sewing sewer(1e-4);
    for (int i = 1; i <= triangleCount; ++i)
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
        if (i % triangleProgressStride == 0)
            ReportImportProgress(progress, "Building B-Rep from mesh...",
                                  MapImportProgress(static_cast<float>(i) / triangleCount, 0.3f, 0.6f));
    }

    ReportImportProgress(progress, "Sewing faces...", 0.6f);
    Handle(GeometryOps::OcctProgressRelay) sewRelay = new GeometryOps::OcctProgressRelay(
        [&](double localProgress01)
        { ReportImportProgress(progress, "Sewing faces...", MapImportProgress(static_cast<float>(localProgress01), 0.6f, 0.8f)); });
    sewer.Perform(sewRelay->Start());
    TopoDS_Shape sewedShape = sewer.SewedShape();

    ReportImportProgress(progress, "Merging coplanar faces...", 0.8f);
    ShapeUpgrade_UnifySameDomain unifier(sewedShape, Standard_True, Standard_True, Standard_True);
    unifier.SetLinearTolerance(1e-4);
    unifier.SetAngularTolerance(1e-3);
    unifier.Build();
    TopoDS_Shape unifiedShape = unifier.Shape();

    ReportImportProgress(progress, "Generating display mesh...", 0.85f);
    BRepMesh_IncrementalMesh meshGen;
    meshGen.SetShape(unifiedShape);
    meshGen.ChangeParameters().Deflection = 0.1;
    meshGen.ChangeParameters().Relative = false;
    meshGen.ChangeParameters().Angle = 0.5;
    meshGen.ChangeParameters().InParallel = true;
    Handle(GeometryOps::OcctProgressRelay) meshRelay = new GeometryOps::OcctProgressRelay(
        [&](double localProgress01)
        {
            ReportImportProgress(progress, "Generating display mesh...",
                                  MapImportProgress(static_cast<float>(localProgress01), 0.85f, 0.9f));
        });
    meshGen.Perform(meshRelay->Start());

    ReportImportProgress(progress, "Populating scene...", 0.9f);
    Solid *solid = scene->CreateSolid({});
    scene->PopulateSolidFromOcctShape(solid, unifiedShape);

    ReportImportProgress(progress, "OBJ Import Complete", 1.0f);
    return true;
}
