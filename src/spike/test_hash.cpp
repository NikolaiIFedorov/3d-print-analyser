#include <TopoDS_Edge.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <gp_Pnt.hxx>
#include <iostream>
#include <unordered_map>
#include <TopoDS.hxx>
#include <TopAbs.hxx>
#include <TopExp.hxx>

int main() {
    TopoDS_Edge e = BRepBuilderAPI_MakeEdge(gp_Pnt(0,0,0), gp_Pnt(1,0,0));
    TopoDS_Edge e_rev = TopoDS::Edge(e.Reversed());
    
    TopoDS_Vertex v1, v2;
    TopExp::Vertices(e, v1, v2);
    
    TopoDS_Vertex v1_rev, v2_rev;
    TopExp::Vertices(e_rev, v1_rev, v2_rev);
    
    std::cout << "v1 orientation: " << v1.Orientation() << std::endl;
    std::cout << "v2 orientation: " << v2.Orientation() << std::endl;
    std::cout << "v1_rev orientation: " << v1_rev.Orientation() << std::endl;
    std::cout << "v2_rev orientation: " << v2_rev.Orientation() << std::endl;
    
    std::cout << "v1 == v1_rev: " << (v1 == v1_rev) << std::endl;
    std::cout << "v1 == v2_rev: " << (v1 == v2_rev) << std::endl;
    
    return 0;
}
