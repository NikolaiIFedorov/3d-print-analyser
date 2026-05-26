#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Face.hxx>
#include <TopTools_ListOfShape.hxx>
#include <iostream>

int main() {
    TopoDS_Solid outer = BRepPrimAPI_MakeBox(10, 10, 10).Solid();
    
    gp_Trsf trsf;
    trsf.SetTranslation(gp_Vec(2, 2, 2));
    BRepBuilderAPI_Transform xform(BRepPrimAPI_MakeBox(6, 6, 6).Solid(), trsf);
    TopoDS_Solid inner = TopoDS::Solid(xform.Shape());
    
    BRepAlgoAPI_Cut cutter(outer, inner);
    cutter.Build();
    TopoDS_Shape hollowBox = cutter.Shape();
    
    TopoDS_Solid emptySolid;
    BRep_Builder B;
    B.MakeSolid(emptySolid);
    
    BRepAlgoAPI_Fuse fuser(hollowBox, emptySolid);
    fuser.Build();
    
    std::cout << "Fuse error: " << fuser.HasErrors() << std::endl;
    
    TopoDS_Shape result = fuser.Shape();
    int solidCount = 0;
    for (TopExp_Explorer ex(result, TopAbs_SOLID); ex.More(); ex.Next()) {
        solidCount++;
    }
    std::cout << "Number of solids: " << solidCount << std::endl;
    
    int faceCount = 0;
    for (TopExp_Explorer ex(result, TopAbs_FACE); ex.More(); ex.Next()) {
        faceCount++;
    }
    std::cout << "Number of faces: " << faceCount << std::endl;
    
    return 0;
}
