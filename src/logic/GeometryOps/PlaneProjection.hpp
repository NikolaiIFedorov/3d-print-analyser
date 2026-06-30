#pragma once

#include <TopoDS_Shape.hxx>

namespace GeometryOps
{

/// Rebuilds each face of `shape` on an explicit world-XY plane (z=0) from its wires.
/// `BRepBuilderAPI_GTransform` (e.g. an affine flatten shear) can leave a planar face whose surface
/// is a non-canonical (transformed) plane that `BRepFilletAPI_MakeFillet2d` rejects as not-planar;
/// rebuilding on an explicit `gp_Pln` restores a real `Geom_Plane` so native fillet ops work, while
/// keeping the analytic edge curves intact. Returns the input unchanged on failure.
TopoDS_Shape ReplanarizeFootprintToXy(const TopoDS_Shape &shape);

} // namespace GeometryOps
