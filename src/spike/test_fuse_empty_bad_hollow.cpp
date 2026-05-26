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
    TopoDS_Solid outer1 = BRepPrimAPI_MakeBox(10, 10, 10).Solid();
    gp_Trsf trsf1;
    trsf1.SetTranslation(gp_Vec(2, 2, 2));
    TopoDS_Solid inner1 = TopoDS::Solid(BRepBuilderAPI_Transform(BRepPrimAPI_MakeBox(6, 6, 6).Solid(), trsf1).Shape());
    BRepAlgoAPI_Cut cutter1(outer1, inner1);
    cutter1.Build();
    TopoDS_Shape hollow1 = cutter1.Shape();
    
    TopoDS_Solid outer2 = BRepPrimAPI_MakeBox(10, 10, 10).Solid();
    gp_Trsf trsf2;
    trsf2.SetTranslation(gp_Vec(2, 2, 2));
    TopoDS_Solid inner2 = TopoDS::Solid(BRepBuilderAPI_Transform(BRepPrimAPI_MakeBox(6, 6, 6).Solid(), trsf2).Shape());
    BRepAlgoAPI_Cut cutter2(outer2, inner2);
    cutter2.Build();
    
    gp_Trsf trsf3;
    trsf3.SetTranslation(gp_Vec(5, 5, 5));
    TopoDS_Shape hollow2 = BRepBuilderAPI_Transform(cutter2.Shape(), trsf3).Shape();
    
    BRepBuilderAPI_Sewing sewer;
    for (TopExp_Explorer ex(hollow1, TopAbs_FACE); ex.More(); ex.Next()) {
        sewer.Add(ex.Current());
    }
    for (TopExp_Explorer ex(hollow2, TopAbs_FACE); ex.More(); ex.Next()) {
        sewer.Add(ex.Current());
    }
    sewer.Perform();
    
    TopoDS_Shape sewed = sewer.SewedShape();
    TopoDS_Solid badSolid;
    BRep_Builder B;
    B.MakeSolid(badSolid);
    for (TopExp_Explorer ex(sewed, TopAbs_SHELL); ex.More(); ex.Next()) {
        B.Add(badSolid, ex.Current());
    }
    
    TopoDS_Solid emptySolid;
    B.MakeSolid(emptySolid);
    
    BRepAlgoAPI_Fuse fuser(badSolid, emptySolid);
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
