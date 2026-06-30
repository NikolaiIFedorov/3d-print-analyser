#pragma once

#include <TopoDS_Shape.hxx>

namespace GeometryOps
{

/// Topology sanity gate for shapes about to be handed to downstream boolean ops / prism extrusion
/// (`BRepAlgoAPI_Cut`, `BRepPrimAPI_MakePrism`). These OCCT builders crash rather than reject
/// cleanly on malformed input (e.g. a closed wire whose start/end junction isn't properly
/// continuous) — `BRepCheck_Analyzer` catches that *before* it reaches them. `context` is logged on
/// failure to identify which call site rejected the shape.
bool ValidateOutlineShapeTopology(const TopoDS_Shape &shape, const char *context);

} // namespace GeometryOps
