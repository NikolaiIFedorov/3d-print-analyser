#include <BOPAlgo_MakerVolume.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
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
    
    // Create a compound of faces from both boxes
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
    mv.Perform();
    
    TopoDS_Shape result = mv.Shape();
    TopTools_ListOfShape solids;
    for (TopExp_Explorer ex(result, TopAbs_SOLID); ex.More(); ex.Next()) {
        solids.Append(ex.Current());
    }
    
    if (solids.Extent() > 1) {
        TopoDS_Shape current = solids.First();
        TopTools_ListIteratorOfListOfShape it(solids);
        it.Next();
        for (; it.More(); it.Next()) {
            BRepAlgoAPI_Fuse fuser(current, it.Value());
            fuser.Build();
            current = fuser.Shape();
        }
        
        int finalSolidCount = 0;
        for (TopExp_Explorer ex(current, TopAbs_SOLID); ex.More(); ex.Next()) {
            finalSolidCount++;
        }
        std::cout << "Final solid count: " << finalSolidCount << std::endl;
        
        int faceCount = 0;
        for (TopExp_Explorer ex(current, TopAbs_FACE); ex.More(); ex.Next()) {
            faceCount++;
        }
        std::cout << "Final face count: " << faceCount << std::endl;
    }
    
    return 0;
}
