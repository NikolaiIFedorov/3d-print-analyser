#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <TopoDS_Wire.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <iostream>

int main() {
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(gp_Pnt(0, 0, 0));
    poly.Add(gp_Pnt(10, 0, 0));
    poly.Add(gp_Pnt(10, 10, 0));
    poly.Add(gp_Pnt(0, 10, 0));
    poly.Close();
    
    TopoDS_Wire wire = poly.Wire();
    
    BRepOffsetAPI_MakeOffset offsetMaker;
    offsetMaker.Init(GeomAbs_Arc);
    offsetMaker.AddWire(wire);
    offsetMaker.Perform(-2.0); // Inset by 2.0
    
    TopoDS_Shape result = offsetMaker.Shape();
    int wireCount = 0;
    for (TopExp_Explorer ex(result, TopAbs_WIRE); ex.More(); ex.Next()) {
        wireCount++;
    }
    std::cout << "Offset wires: " << wireCount << std::endl;
    
    return 0;
}
