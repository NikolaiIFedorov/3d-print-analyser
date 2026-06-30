#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include <iostream>

static void DumpWire(const TopoDS_Wire &wire, const char *label)
{
    std::cout << label << ":\n";
    int i = 0;
    for (BRepTools_WireExplorer ex(wire); ex.More(); ex.Next(), ++i)
    {
        TopoDS_Vertex v1, v2;
        TopExp::Vertices(ex.Current(), v1, v2);
        gp_Pnt p = BRep_Tool::Pnt(v1);
        std::cout << "  " << i << ": (" << p.X() << ", " << p.Y() << ", " << p.Z() << ")\n";
    }
}

int main()
{
    TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();

    TopoDS_Face topFace;
    for (TopExp_Explorer ex(box, TopAbs_FACE); ex.More(); ex.Next())
    {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        BRepAdaptor_Surface adapt(f);
        if (adapt.GetType() == GeomAbs_Plane)
        {
            gp_Dir n = adapt.Plane().Axis().Direction();
            if (f.Orientation() == TopAbs_REVERSED)
                n.Reverse();
            if (n.Z() > 0.9)
            {
                topFace = f;
                break;
            }
        }
    }

    if (topFace.IsNull())
    {
        std::cout << "No top face\n";
        return 1;
    }

    std::cout << "=== Single offset -2.0 ===\n";
    BRepOffsetAPI_MakeOffset single;
    single.Init(topFace, GeomAbs_Arc);
    single.Perform(-2.0);
    if (!single.IsDone())
    {
        std::cout << "Single offset failed\n";
        return 1;
    }
    for (TopExp_Explorer ex(single.Shape(), TopAbs_WIRE); ex.More(); ex.Next())
        DumpWire(TopoDS::Wire(ex.Current()), "wire");

    return 0;
}
