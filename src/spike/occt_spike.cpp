#include <iostream>
#include <chrono>
#include <string>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

// OpenCASCADE includes
#include <STEPControl_Reader.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS.hxx>
#include <TopExp_Explorer.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <Poly_Triangulation.hxx>
#include <BRep_Tool.hxx>
#include <TopoDS_Face.hxx>

// Helper to get current memory usage (Resident Set Size) in MB
double getMemoryUsageMB() {
#ifdef __APPLE__
    struct mach_task_basic_info info;
    mach_msg_type_number_t infoCount = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &infoCount) != KERN_SUCCESS) {
        return 0.0;
    }
    return (double)info.resident_size / (1024.0 * 1024.0);
#else
    return 0.0; // Not implemented for non-macOS in this spike
#endif
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <path_to_step_file>\n";
        return 1;
    }

    std::string filePath = argv[1];
    std::cout << "Starting OpenCASCADE Evaluation Spike\n";
    std::cout << "File: " << filePath << "\n\n";

    double memBaseline = getMemoryUsageMB();
    std::cout << "Baseline Memory: " << memBaseline << " MB\n\n";

    // Step 1: Import
    std::cout << "--- Step 1: Import ---\n";
    auto startImport = std::chrono::high_resolution_clock::now();

    STEPControl_Reader reader;
    IFSelect_ReturnStatus stat = reader.ReadFile(filePath.c_str());
    if (stat != IFSelect_RetDone) {
        std::cerr << "Error: Failed to read STEP file.\n";
        return 1;
    }

    reader.TransferRoots();
    TopoDS_Shape importedShape = reader.OneShape();

    auto endImport = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> importTime = endImport - startImport;
    
    double memAfterImport = getMemoryUsageMB();
    std::cout << "Import Time: " << importTime.count() << " ms\n";
    std::cout << "Memory After Import: " << memAfterImport << " MB (+" << (memAfterImport - memBaseline) << " MB)\n\n";

    if (importedShape.IsNull()) {
        std::cerr << "Error: Imported shape is null.\n";
        return 1;
    }

    // Step 2: Boolean Operation (Self-Intersection Resolution)
    std::cout << "--- Step 2: Boolean Operation ---\n";
    auto startBoolean = std::chrono::high_resolution_clock::now();

    // To resolve self-intersections or intersecting solids, we can fuse all solids together.
    TopoDS_Shape fusedShape;
    bool first = true;
    for (TopExp_Explorer ex(importedShape, TopAbs_SOLID); ex.More(); ex.Next()) {
        TopoDS_Shape solid = ex.Current();
        if (first) {
            fusedShape = solid;
            first = false;
        } else {
            BRepAlgoAPI_Fuse fuse(fusedShape, solid);
            fuse.Build();
            if (fuse.IsDone()) {
                fusedShape = fuse.Shape();
            }
        }
    }

    // If no solids were found, fallback to sewing faces
    if (first) {
        std::cout << "No solids found, sewing faces...\n";
        BRepBuilderAPI_Sewing sewing;
        sewing.Add(importedShape);
        sewing.Perform();
        fusedShape = sewing.SewedShape();
    }

    auto endBoolean = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> booleanTime = endBoolean - startBoolean;
    
    double memAfterBoolean = getMemoryUsageMB();
    std::cout << "Boolean/Sewing Time: " << booleanTime.count() << " ms\n";
    std::cout << "Memory After Boolean: " << memAfterBoolean << " MB (+" << (memAfterBoolean - memAfterImport) << " MB)\n\n";

    // Step 3: Tessellation
    std::cout << "--- Step 3: Tessellation ---\n";
    auto startTessellation = std::chrono::high_resolution_clock::now();

    // Generate mesh with linear deflection 0.1 and angular deflection 0.5
    BRepMesh_IncrementalMesh meshGen(fusedShape, 0.1, false, 0.5, true);

    int totalTriangles = 0;
    int totalVertices = 0;

    for (TopExp_Explorer ex(fusedShape, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face face = TopoDS::Face(ex.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) triangulation = BRep_Tool::Triangulation(face, loc);
        
        if (!triangulation.IsNull()) {
            totalTriangles += triangulation->NbTriangles();
            totalVertices += triangulation->NbNodes();
        }
    }

    auto endTessellation = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> tessellationTime = endTessellation - startTessellation;
    
    double memAfterTessellation = getMemoryUsageMB();
    std::cout << "Tessellation Time: " << tessellationTime.count() << " ms\n";
    std::cout << "Total Vertices: " << totalVertices << "\n";
    std::cout << "Total Triangles: " << totalTriangles << "\n";
    std::cout << "Memory After Tessellation: " << memAfterTessellation << " MB (+" << (memAfterTessellation - memAfterBoolean) << " MB)\n";
    std::cout << "Total Memory Added by File: " << (memAfterTessellation - memBaseline) << " MB\n\n";

    std::cout << "Spike completed successfully.\n";
    return 0;
}
