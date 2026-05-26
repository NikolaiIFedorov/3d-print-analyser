#include "STEPImport.hpp"

#include "scene/scene.hpp"
#include "scene/Geometry/Solid.hpp"
#include "scene/Geometry/Face.hpp"
#include "scene/Geometry/Edge.hpp"
#include "scene/Geometry/Point.hpp"
#include "utils/log.hpp"

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

#include <unordered_map>
#include <vector>
#include <tuple>

namespace {

struct PointHash {
    std::size_t operator()(const glm::dvec3& p) const {
        std::size_t h1 = std::hash<double>{}(p.x);
        std::size_t h2 = std::hash<double>{}(p.y);
        std::size_t h3 = std::hash<double>{}(p.z);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

struct PointEqual {
    bool operator()(const glm::dvec3& a, const glm::dvec3& b) const {
        return a == b;
    }
};

struct EdgeKey {
    Point* a;
    Point* b;
    bool operator==(const EdgeKey& other) const {
        return (a == other.a && b == other.b) || (a == other.b && b == other.a);
    }
};

struct EdgeKeyHash {
    std::size_t operator()(const EdgeKey& k) const {
        std::size_t h1 = std::hash<Point*>{}(k.a);
        std::size_t h2 = std::hash<Point*>{}(k.b);
        return h1 ^ h2;
    }
};

} // namespace

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

    // 3. Tessellation
    // Generate mesh with linear deflection 0.1 and angular deflection 0.5
    BRepMesh_IncrementalMesh meshGen(fusedShape, 0.1, false, 0.5, true);

    // 4. Bridge to Scene
    std::unordered_map<glm::dvec3, Point*, PointHash, PointEqual> pointMap;
    std::unordered_map<EdgeKey, Edge*, EdgeKeyHash> edgeMap;
    std::vector<Face*> sceneFaces;

    auto getOrCreatePoint = [&](const gp_Pnt& p) -> Point* {
        glm::dvec3 pos(p.X(), p.Y(), p.Z());
        auto it = pointMap.find(pos);
        if (it != pointMap.end()) {
            return it->second;
        }
        Point* newPoint = scene->CreatePoint(pos);
        pointMap[pos] = newPoint;
        return newPoint;
    };

    auto getOrCreateEdge = [&](Point* a, Point* b) -> Edge* {
        EdgeKey key{a, b};
        auto it = edgeMap.find(key);
        if (it != edgeMap.end()) {
            return it->second;
        }
        Edge* newEdge = scene->CreateEdge(a, b);
        edgeMap[key] = newEdge;
        return newEdge;
    };

    for (TopExp_Explorer ex(fusedShape, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face face = TopoDS::Face(ex.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) triangulation = BRep_Tool::Triangulation(face, loc);
        
        if (triangulation.IsNull()) continue;

        for (int i = 1; i <= triangulation->NbTriangles(); ++i) {
            const Poly_Triangle& tri = triangulation->Triangle(i);
            int n1, n2, n3;
            tri.Get(n1, n2, n3);

            gp_Pnt p1 = triangulation->Node(n1).Transformed(loc);
            gp_Pnt p2 = triangulation->Node(n2).Transformed(loc);
            gp_Pnt p3 = triangulation->Node(n3).Transformed(loc);

            Point* pt1 = getOrCreatePoint(p1);
            Point* pt2 = getOrCreatePoint(p2);
            Point* pt3 = getOrCreatePoint(p3);

            if (pt1 == pt2 || pt2 == pt3 || pt3 == pt1) {
                continue; // Degenerate triangle
            }

            Edge* e1 = getOrCreateEdge(pt1, pt2);
            Edge* e2 = getOrCreateEdge(pt2, pt3);
            Edge* e3 = getOrCreateEdge(pt3, pt1);

            std::vector<Edge*> loop = {e1, e2, e3};
            Face* sceneFace = scene->CreateFace({loop});
            if (sceneFace != nullptr) {
                sceneFaces.push_back(sceneFace);
            }
        }
    }

    if (sceneFaces.empty()) {
        LOG_ERROR("STEP import resulted in 0 faces after tessellation.");
        return nullptr;
    }

    // Create solid without running topology repairs, to preserve the exact tessellated state
    Solid* solid = scene->CreateSolid(sceneFaces, false);
    LOG_INFO("Successfully imported STEP file. Faces: ", sceneFaces.size());
    return solid;
}
