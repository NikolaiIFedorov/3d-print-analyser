#include <BOPAlgo_MakerVolume.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
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
    
    // Extract faces
    TopTools_ListOfShape faces;
    for (TopExp_Explorer ex(hollowBox, TopAbs_FACE); ex.More(); ex.Next()) {
        faces.Append(ex.Current());
    }
    
    BOPAlgo_MakerVolume mv;
    mv.SetArguments(faces);
    mv.SetIntersect(Standard_True);
    mv.Perform();
    
    TopoDS_Shape result = mv.Shape();
    TopTools_ListOfShape solids;
    for (TopExp_Explorer ex(result, TopAbs_SOLID); ex.More(); ex.Next()) {
        solids.Append(ex.Current());
    }
    
    std::cout << "Number of solids from MakerVolume: " << solids.Extent() << std::endl;
    
    return 0;
}
