// Spike: de-risk the curve-preserving Structure rewrite's single hardest stage — replacing the
// bespoke point-based corner rounding (`FilletRingCorners2D`) with OCCT's native exact-arc
// `BRepFilletAPI_MakeFillet2d`. The carve outlines in `_Structure.stp` are made of lines, circular
// arcs, and ELLIPSE arcs (0 B-splines) — and MakeFillet2d is historically reliable on line/arc
// vertices but can fail where an ellipse meets another edge. This spike loads the real file, takes
// each planar face's actual OCCT wire, insets it the way the carve does (BRepOffsetAPI_MakeOffset
// by -insetMm), and tries to fillet every corner with the real fillet radius (= insetMm = 2.0mm),
// reporting per-face/per-vertex success so we know whether the native path is viable BEFORE
// committing to the larger pipeline restructure.
//
// Build: part of the OCCT-only spike group (see CMakeLists). Run: ./test_fillet2d <file.stp>

#include <iostream>
#include <map>
#include <string>

#include <STEPControl_Reader.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepFilletAPI_MakeFillet2d.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <Geom_Plane.hxx>
#include <gp_Pln.hxx>
#include <Standard_Failure.hxx>

static const char *CurveTypeName(GeomAbs_CurveType t)
{
    switch (t)
    {
    case GeomAbs_Line: return "Line";
    case GeomAbs_Circle: return "Circle";
    case GeomAbs_Ellipse: return "Ellipse";
    case GeomAbs_Hyperbola: return "Hyperbola";
    case GeomAbs_Parabola: return "Parabola";
    case GeomAbs_BezierCurve: return "Bezier";
    case GeomAbs_BSplineCurve: return "BSpline";
    case GeomAbs_OtherCurve: return "Other";
    default: return "?";
    }
}

static std::string EdgeTypeHistogram(const TopoDS_Shape &shape)
{
    std::map<std::string, int> hist;
    for (TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next())
    {
        BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
        hist[CurveTypeName(c.GetType())]++;
    }
    std::string out;
    for (const auto &kv : hist)
        out += (out.empty() ? "" : " ") + kv.first + "x" + std::to_string(kv.second);
    return out.empty() ? "(none)" : out;
}

static size_t CountSub(const TopoDS_Shape &s, TopAbs_ShapeEnum t)
{
    size_t n = 0;
    for (TopExp_Explorer ex(s, t); ex.More(); ex.Next())
        ++n;
    return n;
}

// Try filleting every corner (vertex shared by exactly 2 edges) of `face` at `radius`.
// Reports how many corners were attempted, succeeded, and whether the result stays valid.
static void TryFilletFace(const TopoDS_Face &face, double radius, const char *label)
{
    TopTools_IndexedDataMapOfShapeListOfShape vertToEdges;
    TopExp::MapShapesAndAncestors(face, TopAbs_VERTEX, TopAbs_EDGE, vertToEdges);

    int corners = 0, attempted = 0, succeeded = 0, failed = 0;
    BRepFilletAPI_MakeFillet2d filletMaker(face);
    bool builderAlive = true;

    for (int i = 1; i <= vertToEdges.Extent(); ++i)
    {
        // A roundable corner is a vertex shared by exactly two edges of the wire.
        if (vertToEdges(i).Extent() != 2)
            continue;
        ++corners;
        const TopoDS_Vertex v = TopoDS::Vertex(vertToEdges.FindKey(i));
        try
        {
            ++attempted;
            filletMaker.AddFillet(v, radius);
            // Build incrementally so one bad corner doesn't poison the whole report.
            filletMaker.Build();
            if (filletMaker.IsDone())
                ++succeeded;
            else
            {
                ++failed;
                std::cout << "      [" << label << "] vertex " << i
                          << " AddFillet not done, status=" << static_cast<int>(filletMaker.Status())
                          << "\n";
            }
        }
        catch (const Standard_Failure &e)
        {
            ++failed;
            builderAlive = false;
            std::cout << "      [" << label << "] vertex " << i
                      << " THREW: " << e.GetMessageString() << "\n";
            break; // builder state is now unreliable
        }
    }

    std::cout << "    fillet r=" << radius << "mm [" << label << "]: corners=" << corners
              << " attempted=" << attempted << " succeeded=" << succeeded
              << " failed=" << failed;
    if (builderAlive && filletMaker.IsDone())
    {
        const TopoDS_Shape result = filletMaker.Shape();
        const bool valid = BRepCheck_Analyzer(result).IsValid();
        std::cout << " | result edges=" << CountSub(result, TopAbs_EDGE)
                  << " types=[" << EdgeTypeHistogram(result) << "]"
                  << " valid=" << (valid ? "YES" : "NO");
    }
    else
    {
        std::cout << " | NO USABLE RESULT";
    }
    std::cout << "\n";
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <path_to_step_file> [insetMm=2.0]\n";
        return 1;
    }
    const std::string filePath = argv[1];
    const double insetMm = argc >= 3 ? std::stod(argv[2]) : 2.0;

    STEPControl_Reader reader;
    if (reader.ReadFile(filePath.c_str()) != IFSelect_RetDone)
    {
        std::cerr << "Error: failed to read STEP file.\n";
        return 1;
    }
    reader.TransferRoots();
    TopoDS_Shape shape = reader.OneShape();
    if (shape.IsNull())
    {
        std::cerr << "Error: imported shape is null.\n";
        return 1;
    }

    std::cout << "=== Fillet2d spike on " << filePath << " (insetMm=" << insetMm << ") ===\n";
    std::cout << "Faces in shape: " << CountSub(shape, TopAbs_FACE) << "\n\n";

    int faceIdx = 0;
    int planarFaces = 0, insetOk = 0, filletOkFull = 0;
    for (TopExp_Explorer fex(shape, TopAbs_FACE); fex.More(); fex.Next(), ++faceIdx)
    {
        const TopoDS_Face face = TopoDS::Face(fex.Current());
        BRepAdaptor_Surface surf(face);
        if (surf.GetType() != GeomAbs_Plane)
            continue;
        ++planarFaces;

        const TopoDS_Wire outer = BRepTools::OuterWire(face);
        if (outer.IsNull())
            continue;

        std::cout << "Face #" << faceIdx << " (planar): outer wire edges=" << CountSub(outer, TopAbs_EDGE)
                  << " types=[" << EdgeTypeHistogram(outer) << "]"
                  << " holes=" << (CountSub(face, TopAbs_WIRE) - 1) << "\n";

        // Inset the outer wire the way the carve does (offset inward; arc join keeps it curved).
        TopoDS_Wire insetWire;
        try
        {
            BRepOffsetAPI_MakeOffset off;
            off.Init(GeomAbs_Arc);
            off.AddWire(outer);
            off.Perform(-insetMm);
            if (off.IsDone())
            {
                for (TopExp_Explorer wex(off.Shape(), TopAbs_WIRE); wex.More(); wex.Next())
                {
                    insetWire = TopoDS::Wire(wex.Current());
                    break;
                }
            }
        }
        catch (const Standard_Failure &e)
        {
            std::cout << "    inset THREW: " << e.GetMessageString() << "\n";
        }

        if (insetWire.IsNull())
        {
            std::cout << "    inset failed — skipping fillet for this face\n\n";
            continue;
        }
        ++insetOk;
        std::cout << "    inset OK: edges=" << CountSub(insetWire, TopAbs_EDGE)
                  << " types=[" << EdgeTypeHistogram(insetWire) << "]\n";

        BRepBuilderAPI_MakeFace mkFace(insetWire, Standard_True);
        if (!mkFace.IsDone())
        {
            std::cout << "    MakeFace from inset wire failed — skipping fillet\n\n";
            continue;
        }

        // Fillet at the real radius (= insetMm), and at a smaller radius to separate
        // "radius too large for the edge" failures from "edge type unsupported" failures.
        TryFilletFace(mkFace.Face(), insetMm, "r=inset");
        TryFilletFace(mkFace.Face(), insetMm * 0.25, "r=quarter");
        std::cout << "\n";
    }

    std::cout << "=== Summary: planarFaces=" << planarFaces << " insetOk=" << insetOk << " ===\n";
    return 0;
}
