#include <BOPAlgo_MakerVolume.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBuilderAPI_Transform.hxx>
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
    
    TopTools_ListOfShape faces;
    for (TopExp_Explorer ex(box1, TopAbs_FACE); ex.More(); ex.Next()) {
        faces.Append(ex.Current());
    }
    for (TopExp_Explorer ex(box2, TopAbs_FACE); ex.More(); ex.Next()) {
        faces.Append(ex.Current());
    }
    
    BOPAlgo_MakerVolume mv;
    mv.SetArguments(faces);
    mv.SetIntersect(Standard_True);
    mv.SetAvoidInternalShapes(Standard_True);
    mv.Perform();
    
    TopoDS_Shape result = mv.Shape();
    int solidCount = 0;
    for (TopExp_Explorer ex(result, TopAbs_SOLID); ex.More(); ex.Next()) {
        solidCount++;
    }
    std::cout << "Number of solids: " << solidCount << std::endl;
    
    return 0;
}
