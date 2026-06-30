#include "GeometryOps/PlaneProjection.hpp"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

#include <vector>

namespace GeometryOps
{

// Rebuilds each face of `shape` on an explicit world-XY plane (z=0) from its wires. BRepBuilderAPI_GTransform
// (the flatten shear) leaves a planar face whose surface is a non-canonical (transformed) plane that
// BRepFilletAPI_MakeFillet2d rejects as not-planar; rebuilding on an explicit gp_Pln restores a real
// Geom_Plane so the native fillet works — while keeping the analytic edge curves. Returns input on failure.
TopoDS_Shape ReplanarizeFootprintToXy(const TopoDS_Shape &shape)
{
    if (shape.IsNull())
        return shape;
    const gp_Pln xyPlane(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0));
    std::vector<TopoDS_Face> rebuilt;
    for (TopExp_Explorer fexp(shape, TopAbs_FACE); fexp.More(); fexp.Next())
    {
        const TopoDS_Face f = TopoDS::Face(fexp.Current());
        const TopoDS_Wire outer = BRepTools::OuterWire(f);
        if (outer.IsNull())
        {
            rebuilt.push_back(f);
            continue;
        }
        BRepBuilderAPI_MakeFace mk(xyPlane, outer);
        for (TopExp_Explorer wexp(f, TopAbs_WIRE); wexp.More(); wexp.Next())
        {
            const TopoDS_Wire w = TopoDS::Wire(wexp.Current());
            if (!w.IsSame(outer))
                mk.Add(w);
        }
        rebuilt.push_back(mk.IsDone() ? mk.Face() : f);
    }
    if (rebuilt.empty())
        return shape;
    if (rebuilt.size() == 1)
        return rebuilt.front();
    BRep_Builder builder;
    TopoDS_Compound out;
    builder.MakeCompound(out);
    for (const TopoDS_Face &f : rebuilt)
        builder.Add(out, f);
    return out;
}

} // namespace GeometryOps
