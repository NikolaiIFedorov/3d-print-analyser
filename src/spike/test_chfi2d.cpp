// Spike: can ChFi2d_FilletAPI round the ARC-ADJACENT corners that BRepFilletAPI_MakeFillet2d refused
// (ChFi2d_NotAuthorized), and can we reassemble a valid wire from the per-corner results?
//
// MakeFillet2d (the convenience wrapper) bails with NotAuthorized when a corner's neighbour edge is an
// arc — and our footprints are full of arcs (offset convex-corner rounding + ellipse/circle holes).
// The lower-level ChFi2d_FilletAPI has no such guard and handles line/arc/ellipse/b-spline corners,
// but works one corner at a time (2 edges in → fillet edge + 2 trimmed edges out), so the wire must be
// rebuilt manually. This spike prototypes exactly that on _Structure.stp's real inset wires and reports
// how many corners round and whether the rebuilt wire is valid — before committing to it in the carve.
//
// Build: target test_chfi2d. Run: ./test_chfi2d <file.stp> [insetMm=2.0]

#include <iostream>
#include <vector>

#include <STEPControl_Reader.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <ChFi2d_FilletAPI.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <gp_Pln.hxx>
#include <Standard_Failure.hxx>

static const char *CurveTypeName(GeomAbs_CurveType t)
{
    switch (t)
    {
    case GeomAbs_Line: return "Line";
    case GeomAbs_Circle: return "Circle";
    case GeomAbs_Ellipse: return "Ellipse";
    case GeomAbs_BSplineCurve: return "BSpline";
    default: return "Other";
    }
}

static double EdgeLength(const TopoDS_Edge &e)
{
    GProp_GProps props;
    BRepGProp::LinearProperties(e, props);
    return props.Mass();
}

// Fillet every corner of a closed wire using the per-corner ChFi2d_FilletAPI, then rebuild a wire.
static void TryChFi2dWholeWire(const TopoDS_Wire &wire, const gp_Pln &plane, double radiusMm,
                               const TopoDS_Face &faceForOrder)
{
    std::vector<TopoDS_Edge> edges;
    for (BRepTools_WireExplorer ex(wire, faceForOrder); ex.More(); ex.Next())
        edges.push_back(ex.Current());
    const size_t n = edges.size();
    if (n < 2)
    {
        std::cout << "    [chfi2d] wire has <2 edges; skip\n";
        return;
    }

    // Shared vertex (corner point) between edge i and edge i+1.
    std::vector<gp_Pnt> cornerPt(n);
    std::vector<bool> hasCorner(n, false);
    for (size_t i = 0; i < n; ++i)
    {
        TopoDS_Vertex v;
        if (TopExp::CommonVertex(edges[i], edges[(i + 1) % n], v))
        {
            cornerPt[i] = BRep_Tool::Pnt(v);
            hasCorner[i] = true;
        }
    }

    std::vector<TopoDS_Edge> cur = edges;  // edges get trimmed in place as corners are filleted
    std::vector<TopoDS_Edge> arc(n);       // fillet arc at corner i (between edge i and i+1)
    int corners = 0, filleted = 0, failed = 0;

    for (size_t i = 0; i < n; ++i)
    {
        if (!hasCorner[i] || cur[i].IsNull() || cur[(i + 1) % n].IsNull())
            continue;
        ++corners;
        // Per-corner radius cap to the shorter adjacent edge, with a small shrink ladder.
        const double cap = std::min(radiusMm, 0.45 * std::min(EdgeLength(cur[i]), EdgeLength(cur[(i + 1) % n])));
        bool ok = false;
        for (double r = cap; r >= radiusMm * 0.02 && !ok; r *= 0.6)
        {
            try
            {
                ChFi2d_FilletAPI api(cur[i], cur[(i + 1) % n], plane);
                if (!api.Perform(r))
                    continue;
                if (api.NbResults(cornerPt[i]) <= 0)
                    continue;
                TopoDS_Edge m1, m2;
                const TopoDS_Edge f = api.Result(cornerPt[i], m1, m2, -1);
                if (f.IsNull() || m1.IsNull() || m2.IsNull())
                    continue;
                cur[i] = m1;
                cur[(i + 1) % n] = m2;
                arc[i] = f;
                ok = true;
            }
            catch (const Standard_Failure &)
            {
            }
        }
        if (ok)
            ++filleted;
        else
            ++failed;
    }

    // Reassemble: edge0, arc0, edge1, arc1, ... (arc(n-1) closes back to edge0).
    BRepBuilderAPI_MakeWire mkWire;
    for (size_t i = 0; i < n; ++i)
    {
        if (!cur[i].IsNull())
            mkWire.Add(cur[i]);
        if (!arc[i].IsNull())
            mkWire.Add(arc[i]);
    }
    const bool wireDone = mkWire.IsDone();
    bool wireValid = false, faceValid = false;
    if (wireDone)
    {
        wireValid = BRepCheck_Analyzer(mkWire.Wire()).IsValid();
        BRepBuilderAPI_MakeFace mkFace(plane, mkWire.Wire());
        if (mkFace.IsDone())
            faceValid = BRepCheck_Analyzer(mkFace.Face()).IsValid();
    }

    std::cout << "    [chfi2d] corners=" << corners << " filleted=" << filleted << " failed=" << failed
              << " | rebuilt wireDone=" << wireDone << " wireValid=" << wireValid
              << " faceValid=" << faceValid << "\n";
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
    const TopoDS_Shape shape = reader.OneShape();
    if (shape.IsNull())
        return 1;

    std::cout << "=== ChFi2d arc-fillet spike on " << filePath << " (insetMm=" << insetMm << ") ===\n";

    int faceIdx = 0;
    for (TopExp_Explorer fex(shape, TopAbs_FACE); fex.More(); fex.Next(), ++faceIdx)
    {
        const TopoDS_Face face = TopoDS::Face(fex.Current());
        BRepAdaptor_Surface surf(face);
        if (surf.GetType() != GeomAbs_Plane)
            continue;
        const TopoDS_Wire outer = BRepTools::OuterWire(face);
        if (outer.IsNull())
            continue;

        // Inset the outer wire (arc-rounded convex corners — the case MakeFillet2d choked on).
        TopoDS_Wire insetWire;
        try
        {
            BRepOffsetAPI_MakeOffset off;
            off.Init(GeomAbs_Arc);
            off.AddWire(outer);
            off.Perform(-insetMm);
            if (off.IsDone())
                for (TopExp_Explorer wex(off.Shape(), TopAbs_WIRE); wex.More(); wex.Next())
                {
                    insetWire = TopoDS::Wire(wex.Current());
                    break;
                }
        }
        catch (const Standard_Failure &e)
        {
            std::cout << "Face #" << faceIdx << " inset THREW: " << e.GetMessageString() << "\n";
            continue;
        }
        if (insetWire.IsNull())
            continue;

        // Edge-type histogram of the inset wire (shows the arcs that defeat MakeFillet2d).
        int nLine = 0, nCirc = 0, nOther = 0;
        for (TopExp_Explorer ex(insetWire, TopAbs_EDGE); ex.More(); ex.Next())
        {
            BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
            if (c.GetType() == GeomAbs_Line) ++nLine;
            else if (c.GetType() == GeomAbs_Circle) ++nCirc;
            else ++nOther;
        }
        std::cout << "Face #" << faceIdx << " inset edges: Line=" << nLine << " Circle=" << nCirc
                  << " Other=" << nOther << "\n";

        const gp_Pln plane = surf.Plane();
        BRepBuilderAPI_MakeFace mkInsetFace(plane, insetWire);
        if (!mkInsetFace.IsDone())
        {
            std::cout << "    inset MakeFace failed; skip\n";
            continue;
        }
        TryChFi2dWholeWire(insetWire, plane, insetMm, mkInsetFace.Face());
    }
    return 0;
}
