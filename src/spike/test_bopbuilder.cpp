#include <BOPAlgo_Builder.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
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
    TopoDS_Solid box1 = BRepPrimAPI_MakeBox(10, 10, 10).Solid();
    
    gp_Trsf trsf;
    trsf.SetTranslation(gp_Vec(5, 5, 5));
    BRepBuilderAPI_Transform xform(box1, trsf);
    TopoDS_Solid box2 = TopoDS::Solid(xform.Shape());
    
    // Create a single shell from both boxes to simulate a self-intersecting solid
    BRepBuilderAPI_Sewing sewer;
    for (TopExp_Explorer ex(box1, TopAbs_FACE); ex.More(); ex.Next()) {
        sewer.Add(ex.Current());
    }
    for (TopExp_Explorer ex(box2, TopAbs_FACE); ex.More(); ex.Next()) {
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
    
    BOPAlgo_Builder builder;
    builder.AddArgument(badSolid);
    builder.Perform();
    
    std::cout << "Builder error: " << builder.HasErrors() << std::endl;
    
    TopoDS_Shape result = builder.Shape();
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
