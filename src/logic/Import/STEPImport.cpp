#include "STEPImport.hpp"

#include "scene/scene.hpp"
#include "scene/Geometry/Solid.hpp"
#include "scene/Geometry/Face.hpp"
#include "scene/Geometry/Edge.hpp"
#include "scene/Geometry/Point.hpp"
#include "scene/Geometry/Surface.hpp"
#include "utils/log.hpp"

// OpenCASCADE includes
#include <STEPControl_Reader.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS.hxx>
#include <TopExp_Explorer.hxx>
#include <TopExp.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>

#include <unordered_map>
#include <vector>



Solid *ImportSTEP(Scene *scene, const std::string &filepath)
{
    if (scene == nullptr)
        return nullptr;

    LOG_INFO("Importing STEP file via OpenCASCADE: ", filepath);

    // 1. Read STEP file
    STEPControl_Reader reader;
    IFSelect_ReturnStatus stat = reader.ReadFile(filepath.c_str());
    if (stat != IFSelect_RetDone) {
        LOG_ERROR("Failed to read STEP file: ", filepath);
        return nullptr;
    }

    reader.TransferRoots();
    TopoDS_Shape importedShape = reader.OneShape();
    if (importedShape.IsNull()) {
        LOG_ERROR("Imported STEP shape is null.");
        return nullptr;
    }

    // 2. Boolean Operation (Self-Intersection Resolution)
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
        BRepBuilderAPI_Sewing sewing;
        sewing.Add(importedShape);
        sewing.Perform();
        fusedShape = sewing.SewedShape();
    }

    // 3. Tessellation (required for rendering later)
    BRepMesh_IncrementalMesh meshGen(fusedShape, 0.1, false, 0.5, true);

    // 4. Bridge to Scene (B-Rep Translation)
    std::unordered_map<TopoDS_Shape, Point*> pointMap;
    std::unordered_map<TopoDS_Shape, Edge*> edgeMap;
    std::vector<Face*> sceneFaces;

    auto getOrCreatePoint = [&](const TopoDS_Vertex& v) -> Point* {
        auto it = pointMap.find(v);
        if (it != pointMap.end()) {
            return it->second;
        }
        gp_Pnt p = BRep_Tool::Pnt(v);
        glm::dvec3 pos(p.X(), p.Y(), p.Z());
        Point* newPoint = scene->CreatePoint(pos);
        newPoint->occtVertex = v;
        pointMap[v] = newPoint;
        return newPoint;
    };

    auto getOrCreateEdge = [&](const TopoDS_Edge& e) -> Edge* {
        auto it = edgeMap.find(e);
        if (it != edgeMap.end()) {
            return it->second;
        }
        TopoDS_Vertex v1, v2;
        TopExp::Vertices(e, v1, v2);
        
        Point* p1 = getOrCreatePoint(v1);
        Point* p2 = getOrCreatePoint(v2);

        Edge* newEdge = scene->CreateEdge(p1, p2);
        newEdge->occtEdge = e;
        edgeMap[e] = newEdge;
        return newEdge;
    };

    for (TopExp_Explorer exFace(fusedShape, TopAbs_FACE); exFace.More(); exFace.Next()) {
        TopoDS_Face occtFace = TopoDS::Face(exFace.Current());
        
        std::vector<std::vector<Edge*>> loops;

        for (TopExp_Explorer exWire(occtFace, TopAbs_WIRE); exWire.More(); exWire.Next()) {
            TopoDS_Wire wire = TopoDS::Wire(exWire.Current());
            std::vector<Edge*> loopEdges;
            
            for (BRepTools_WireExplorer exEdge(wire); exEdge.More(); exEdge.Next()) {
                TopoDS_Edge occtEdge = exEdge.Current();
                Edge* edge = getOrCreateEdge(occtEdge);
                loopEdges.push_back(edge);
            }
            if (!loopEdges.empty()) {
                loops.push_back(loopEdges);
            }
        }

        if (loops.empty()) continue;

        auto surf = std::make_unique<OcctSurface>(occtFace);
        Face* sceneFace = scene->CreateFace(loops, std::move(surf));
        if (sceneFace != nullptr) {
            sceneFace->occtFace = occtFace;
            sceneFaces.push_back(sceneFace);
        }
    }

    if (sceneFaces.empty()) {
        LOG_ERROR("STEP import resulted in 0 faces.");
        return nullptr;
    }

    Solid* solid = scene->CreateSolid(sceneFaces, false);
    solid->occtShape = fusedShape;
    LOG_INFO("Successfully imported STEP file. Faces: ", sceneFaces.size());
    return solid;
}
