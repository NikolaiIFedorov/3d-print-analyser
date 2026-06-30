#include "GeometryOps/Fillet2D.hpp"

#include "utils/log.hpp"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <ChFi2d_FilletAPI.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_Shape.hxx>
#include <ShapeFix_Face.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <vector>

namespace GeometryOps
{

double FilletEdgeLengthMm(const TopoDS_Edge &e)
{
    GProp_GProps props;
    BRepGProp::LinearProperties(e, props);
    return props.Mass();
}

// Change C: round a closed planar wire's corners with the LOWER-LEVEL ChFi2d_FilletAPI, which (unlike
// BRepFilletAPI_MakeFillet2d) fillets line/arc/ellipse/b-spline corners — including the arc-adjacent
// ones the wrapper refuses with ChFi2d_NotAuthorized. It works one corner at a time (2 edges → fillet
// arc + 2 trimmed edges), so the wire is rebuilt manually. Each corner is committed only if the
// rebuilt wire still forms a VALID face (a temp MakeFace catches the self-intersections that a
// wire-only BRepCheck misses), trying a shrinking per-corner radius ladder capped to ~45% of the
// shorter adjacent edge — so adjacent fillets never overlap and tight/tangent corners are skipped
// cleanly. Returns the original wire if nothing rounds.
TopoDS_Wire FilletWireWithChFi2d(const TopoDS_Wire &wire, const gp_Pln &plane, double radiusMm)
{
    // Per-corner radius ladder: start capped at kFilletRadiusCapFraction of the shorter adjacent edge
    // (so neighbouring fillets can't overlap), shrink by kFilletRadiusShrinkFactor each retry until
    // either a valid fillet is found or the radius drops below kFilletRadiusFloorFraction * radiusMm
    // (below which the corner is left sharp rather than rounded).
    constexpr double kFilletRadiusCapFraction = 0.45;
    constexpr double kFilletRadiusShrinkFactor = 0.6;
    constexpr double kFilletRadiusFloorFraction = 0.02;

    std::vector<TopoDS_Edge> edges;
    for (BRepTools_WireExplorer ex(wire); ex.More(); ex.Next())
        edges.push_back(ex.Current());
    const size_t n = edges.size();
    if (n < 2)
        return wire;

    // Shared corner point between edge i and edge (i+1)%n.
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

    std::vector<TopoDS_Edge> cur = edges; // trimmed in place as corners round
    std::vector<TopoDS_Edge> arc(n);      // fillet arc at corner i

    auto buildWire = [&](const std::vector<TopoDS_Edge> &c, const std::vector<TopoDS_Edge> &a) -> TopoDS_Wire
    {
        BRepBuilderAPI_MakeWire mk;
        for (size_t i = 0; i < n; ++i)
        {
            if (!c[i].IsNull())
                mk.Add(c[i]);
            if (!a[i].IsNull())
                mk.Add(a[i]);
        }
        return mk.IsDone() ? mk.Wire() : TopoDS_Wire();
    };
    auto formsValidFace = [&](const TopoDS_Wire &w) -> bool
    {
        if (w.IsNull())
            return false;
        BRepBuilderAPI_MakeFace mk(plane, w);
        return mk.IsDone() && BRepCheck_Analyzer(mk.Face()).IsValid();
    };

    for (size_t i = 0; i < n; ++i)
    {
        const size_t j = (i + 1) % n;
        if (!hasCorner[i] || cur[i].IsNull() || cur[j].IsNull())
            continue;
        const double cap = std::min(radiusMm, kFilletRadiusCapFraction *
                                                   std::min(FilletEdgeLengthMm(cur[i]), FilletEdgeLengthMm(cur[j])));
        for (double r = cap; r >= radiusMm * kFilletRadiusFloorFraction; r *= kFilletRadiusShrinkFactor)
        {
            TopoDS_Edge m1, m2, filletEdge;
            try
            {
                ChFi2d_FilletAPI api(cur[i], cur[j], plane);
                if (!api.Perform(r) || api.NbResults(cornerPt[i]) <= 0)
                    continue;
                filletEdge = api.Result(cornerPt[i], m1, m2, -1);
            }
            catch (const Standard_Failure &e)
            {
                LOG_DEBU("GeometryOps fillet: ChFi2d_FilletAPI threw at r=", r, ":", e.GetMessageString());
                continue;
            }
            if (filletEdge.IsNull() || m1.IsNull() || m2.IsNull())
                continue;

            std::vector<TopoDS_Edge> tc = cur;
            std::vector<TopoDS_Edge> ta = arc;
            tc[i] = m1;
            tc[j] = m2;
            ta[i] = filletEdge;
            const TopoDS_Wire cand = buildWire(tc, ta);
            // Commit only if the result still forms a valid face (catches self-intersection a
            // wire-only check misses); otherwise try a smaller radius, then leave the corner sharp.
            if (formsValidFace(cand))
            {
                cur = std::move(tc);
                arc = std::move(ta);
                break;
            }
        }
    }

    const TopoDS_Wire result = buildWire(cur, arc);
    return result.IsNull() ? wire : result;
}

// Fillets every wire (outer + holes) of a planar footprint face and rebuilds the face. Falls back to
// the original face if the rebuild is invalid, so a bad fillet degrades to sharp rather than breaking.
TopoDS_Face FilletFaceWithChFi2d(const TopoDS_Face &face, double radiusMm)
{
    if (face.IsNull() || radiusMm <= 1e-6)
        return face;

    gp_Pln plane(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0));
    BRepAdaptor_Surface surf(face);
    if (surf.GetType() == GeomAbs_Plane)
        plane = surf.Plane();

    const TopoDS_Wire outer = BRepTools::OuterWire(face);
    if (outer.IsNull())
        return face;

    const TopoDS_Wire newOuter = FilletWireWithChFi2d(outer, plane, radiusMm);
    BRepBuilderAPI_MakeFace mk(plane, newOuter.IsNull() ? outer : newOuter);
    for (TopExp_Explorer wexp(face, TopAbs_WIRE); wexp.More(); wexp.Next())
    {
        const TopoDS_Wire w = TopoDS::Wire(wexp.Current());
        if (w.IsSame(outer))
            continue;
        const TopoDS_Wire newHole = FilletWireWithChFi2d(w, plane, radiusMm);
        mk.Add(newHole.IsNull() ? w : newHole);
    }
    if (mk.IsDone())
    {
        const TopoDS_Face cand = mk.Face();
        // Each wire was only validated in isolation (`formsValidFace` builds a single-wire face),
        // so the combined outer+holes face can still be invalid — in practice because
        // BRepBuilderAPI_MakeWire re-derives a rebuilt hole loop's orientation, which can come out
        // flipped relative to the outer wire once any of its corners actually round. An invalid
        // face here used to escape downstream, fail the prism's topology check, and silently drop
        // the whole curved carve to the polygon-discretized fallback (fragmented top faces,
        // mangled struts). Validate, repair wire senses if needed, and only then accept.
        if (BRepCheck_Analyzer(cand).IsValid())
            return cand;
        ShapeFix_Face fixer(cand);
        fixer.FixOrientationMode() = 1;
        fixer.Perform();
        const TopoDS_Face repaired = fixer.Face();
        if (!repaired.IsNull() && BRepCheck_Analyzer(repaired).IsValid())
            return repaired;
    }
    return face; // fall back to un-filleted on any failure
}

TopoDS_Shape FilletCurvedFootprint(const TopoDS_Shape &footprint, double radiusMm)
{
    if (footprint.IsNull() || radiusMm <= 1e-6)
        return footprint;

    std::vector<TopoDS_Face> filletedFaces;
    for (TopExp_Explorer fexp(footprint, TopAbs_FACE); fexp.More(); fexp.Next())
    {
        const TopoDS_Face filleted = FilletFaceWithChFi2d(TopoDS::Face(fexp.Current()), radiusMm);
        if (!filleted.IsNull())
            filletedFaces.push_back(filleted);
    }
    if (filletedFaces.empty())
        return footprint;
    if (filletedFaces.size() == 1)
        return filletedFaces.front();

    BRep_Builder builder;
    TopoDS_Compound out;
    builder.MakeCompound(out);
    for (const TopoDS_Face &f : filletedFaces)
        builder.Add(out, f);
    return out;
}

} // namespace GeometryOps
