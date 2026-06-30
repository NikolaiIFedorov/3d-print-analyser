#pragma once

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>

class gp_Pln;

namespace GeometryOps
{

double FilletEdgeLengthMm(const TopoDS_Edge &e);

/// Rounds a closed planar wire's corners with the lower-level `ChFi2d_FilletAPI`, which (unlike
/// `BRepFilletAPI_MakeFillet2d`) fillets line/arc/ellipse/b-spline corners, including arc-adjacent
/// ones the higher-level wrapper refuses. Each corner is committed only if the rebuilt wire still
/// forms a valid face, trying a shrinking per-corner radius ladder so adjacent fillets never overlap
/// and tight/tangent corners are skipped cleanly. Returns the original wire if nothing rounds.
TopoDS_Wire FilletWireWithChFi2d(const TopoDS_Wire &wire, const gp_Pln &plane, double radiusMm);

/// Fillets every wire (outer + holes) of a planar footprint face and rebuilds the face. Falls back
/// to the original face if the rebuild is invalid, so a bad fillet degrades to sharp rather than
/// breaking the caller's pipeline.
TopoDS_Face FilletFaceWithChFi2d(const TopoDS_Face &face, double radiusMm);

/// Applies `FilletFaceWithChFi2d` to every face of `footprint` (a face or compound of faces).
TopoDS_Shape FilletCurvedFootprint(const TopoDS_Shape &footprint, double radiusMm);

} // namespace GeometryOps
