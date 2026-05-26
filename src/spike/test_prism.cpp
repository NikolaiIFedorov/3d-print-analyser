#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Vec.hxx>
#include <iostream>

int main() {
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(gp_Pnt(0, 0, 0));
    poly.Add(gp_Pnt(10, 0, 0));
    poly.Add(gp_Pnt(10, 10, 0));
    poly.Add(gp_Pnt(0, 10, 0));
    poly.Close();
    
    TopoDS_Face face = BRepBuilderAPI_MakeFace(poly.Wire());
    TopoDS_Shape prism = BRepPrimAPI_MakePrism(face, gp_Vec(0, 0, 10));
    
    std::cout << "Prism is null: " << prism.IsNull() << std::endl;
    return 0;
}
