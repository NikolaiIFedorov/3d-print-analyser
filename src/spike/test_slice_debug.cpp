// Diagnostic spike for the Analysis-tool slicing bug report: a round/curved hole shows up as a
// straight chord at some Z layers in the Analysis debug overlay. Loads a STEP file the same way
// the app does (ImportSTEP) and dumps, per face, its surface/edge geometry types, then slices the
// solid layer-by-layer and reports any loop whose ring has suspiciously few points (a real circle
// tessellated at 0.05mm chord tolerance should have dozens of points; a chorded "circle" will have
// as few as the edge count in its loop).

#include "Import/STEPImport.hpp"
#include "Analysis/utils/Slice.hpp"
#include "GeometryOps/RingUtils.hpp"
#include "scene/scene.hpp"
#include "scene/Geometry/Face.hpp"
#include "scene/Geometry/Edge.hpp"
#include "scene/Geometry/Surface.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <GeomAbs_CurveType.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Pln.hxx>

#include <cstdio>
#include <cstdlib>
#include <string>

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
    case GeomAbs_OffsetCurve: return "Offset";
    default: return "Other";
    }
}

int main(int argc, char **argv)
{
    std::string path = argc > 1 ? argv[1] : "/Users/nikolnotai/Documents/_Structure.stp";

    Scene scene;
    Solid *solid = ImportSTEP(&scene, path);
    if (solid == nullptr)
    {
        std::printf("Failed to import %s\n", path.c_str());
        return 1;
    }

    std::printf("Imported solid: %zu faces\n", solid->faces.size());

    for (size_t fi = 0; fi < solid->faces.size(); fi++)
    {
        Face *face = solid->faces[fi];
        if (face == nullptr || !face->HasGeometry())
            continue;
        bool planar = face->GetSurface().IsPlanar();
        bool occtFaceNull = face->occtFace.IsNull();
        std::printf("Face %zu: planar=%d occtFaceNull=%d loops=%zu\n", fi, planar, occtFaceNull,
                    face->loops.size());
        for (size_t li = 0; li < face->loops.size(); li++)
        {
            const auto &loop = face->loops[li];
            std::printf("  loop %zu: %zu edges\n", li, loop.size());
            for (size_t ei = 0; ei < loop.size(); ei++)
            {
                const Edge *edge = loop[ei].edge;
                if (edge == nullptr)
                {
                    std::printf("    edge %zu: NULL\n", ei);
                    continue;
                }
                bool occtEdgeNull = edge->occtEdge.IsNull();
                const char *curveTypeStr = "occtEdgeNull";
                if (!occtEdgeNull)
                {
                    BRepAdaptor_Curve adaptor(edge->occtEdge);
                    curveTypeStr = CurveTypeName(adaptor.GetType());
                }
                std::printf("    edge %zu: curveAttachedNull=%d occtEdgeNull=%d occtCurveType=%s start=(%.3f,%.3f,%.3f) end=(%.3f,%.3f,%.3f)\n",
                            ei, edge->curve == nullptr, occtEdgeNull, curveTypeStr,
                            edge->startPoint->position.x, edge->startPoint->position.y, edge->startPoint->position.z,
                            edge->endPoint->position.x, edge->endPoint->position.y, edge->endPoint->position.z);
            }
        }
    }

    ZBounds zb = Slice::GetZBounds(solid);
    std::printf("\nZ bounds: [%.4f, %.4f]\n", zb.zMin, zb.zMax);

    const double layerHeight = 0.2;
    int layerIndex = 0;
    for (double z = zb.zMin + layerHeight; z < zb.zMax; z += layerHeight, layerIndex++)
    {
        std::vector<Segment> segs = Slice::At(solid, z);
        std::vector<SliceLoop> loops = Slice::ExtractLoops(segs);
        for (size_t li = 0; li < loops.size(); li++)
        {
            const SliceLoop &loop = loops[li];
            if (!loop.isHole)
                std::printf("layer %d z=%.4f OUTER loop %zu ringPts=%zu\n", layerIndex, z, li, loop.ring.size());
            if (loop.ring.size() <= 6)
            {
                double minX = loop.ring[0].x, maxX = loop.ring[0].x;
                double minY = loop.ring[0].y, maxY = loop.ring[0].y;
                for (const auto &p : loop.ring)
                {
                    minX = std::min(minX, p.x);
                    maxX = std::max(maxX, p.x);
                    minY = std::min(minY, p.y);
                    maxY = std::max(maxY, p.y);
                }
                std::printf("layer %d z=%.4f loop %zu isHole=%d ringPts=%zu bbox=(%.3f..%.3f, %.3f..%.3f)\n",
                            layerIndex, z, li, loop.isHole, loop.ring.size(), minX, maxX, minY, maxY);
            }
        }
    }

    std::printf("\nTotal layers: %d\n", layerIndex);

    // Dump every raw segment + extracted loop at a probe Z, tagged by which face produced it, to
    // see whether the curved face's section is being tessellated into many short segments
    // (correct) or just one long chord (still broken) — and whether ExtractLoops is dropping an
    // otherwise-valid loop (open chain, doesn't close back to start).
    const double probeZ = argc > 2 ? std::atof(argv[2]) : 8.6;
    std::vector<Segment> probeSegs = Slice::At(solid, probeZ);
    std::printf("\nRaw segments at z=%.4f (%zu total):\n", probeZ, probeSegs.size());
    for (size_t si = 0; si < probeSegs.size(); si++)
    {
        const Segment &s = probeSegs[si];
        int faceIdx = -1;
        for (size_t fi = 0; fi < solid->faces.size(); fi++)
        {
            if (solid->faces[fi] == s.face)
            {
                faceIdx = static_cast<int>(fi);
                break;
            }
        }
        std::printf("  seg %zu: face=%d isHole=%d a=(%.8f,%.8f,%.8f) b=(%.8f,%.8f,%.8f) len=%.4f\n", si, faceIdx,
                    s.isHole, s.a.x, s.a.y, s.a.z, s.b.x, s.b.y, s.b.z,
                    std::sqrt((s.a.x - s.b.x) * (s.a.x - s.b.x) + (s.a.y - s.b.y) * (s.a.y - s.b.y)));
    }

    // Isolation check: does ExtractLoops succeed on just the notch's segments (faces 10/11/12)
    // alone, or does it fail even without the rest of the solid's segments present?
    std::vector<Segment> notchOnly;
    for (const Segment &s : probeSegs)
    {
        int faceIdx = -1;
        for (size_t fi = 0; fi < solid->faces.size(); fi++)
            if (solid->faces[fi] == s.face) { faceIdx = static_cast<int>(fi); break; }
        if (faceIdx == 10 || faceIdx == 11 || faceIdx == 12)
            notchOnly.push_back(s);
    }
    std::vector<SliceLoop> notchOnlyLoops = Slice::ExtractLoops(notchOnly);
    std::printf("\nIsolated notch (faces 10/11/12) segments: %zu, loops found: %zu\n", notchOnly.size(),
                notchOnlyLoops.size());
    for (size_t li = 0; li < notchOnlyLoops.size(); li++)
        std::printf("  isolated loop %zu: isHole=%d ringPts=%zu\n", li, notchOnlyLoops[li].isHole,
                    notchOnlyLoops[li].ring.size());

    // Vertex-degree check: in a simple closed loop every endpoint should be shared by exactly 2
    // segments. A degree != 2 vertex means either a dangling end (degree 1, the chain doesn't
    // close) or a branch point (degree > 2, an unexpected extra coincident point).
    std::printf("\nNotch endpoint degrees (xy distance < 1e-6 => same vertex):\n");
    std::vector<glm::dvec3> endpoints;
    for (const Segment &s : notchOnly)
    {
        endpoints.push_back(s.a);
        endpoints.push_back(s.b);
    }
    for (size_t i = 0; i < endpoints.size(); i++)
    {
        int degree = 0;
        for (size_t j = 0; j < endpoints.size(); j++)
            if (glm::length(glm::dvec2(endpoints[i] - endpoints[j])) < 1e-6)
                degree++;
        if (degree != 2)
            std::printf("  endpoint %zu (%.8f,%.8f) has degree %d\n", i, endpoints[i].x, endpoints[i].y, degree);
    }
    std::printf("  (no output above this line means every endpoint has degree 2)\n");

    std::printf("\nMinimum nonzero pairwise xy gap between distinct-looking endpoints (quantifies the\n"
                "actual float mismatch between the two numerical pipelines):\n");
    for (size_t i = 0; i < endpoints.size(); i++)
    {
        double bestNonzero = -1.0;
        for (size_t j = 0; j < endpoints.size(); j++)
        {
            if (i == j)
                continue;
            double d = glm::length(glm::dvec2(endpoints[i] - endpoints[j]));
            if (d < 1e-6 && (bestNonzero < 0.0 || d < bestNonzero))
                bestNonzero = d;
        }
        if (bestNonzero > 0.0)
            std::printf("  endpoint %zu: nearest-match gap = %.15e\n", i, bestNonzero);
    }

    // Directly replicate AppendCurvedFaceSegments for face 11 to see if BRepAlgoAPI_Section is
    // itself splitting the section curve into multiple disjoint edges with a real gap between them,
    // and whether a fuzzy tolerance on the section operation avoids the split at the source.
    for (double fuzzy : {0.0, 1e-6, 1e-4, 1e-3, 1e-2})
    {
        Face *face11 = solid->faces[11];
        const gp_Pln plane(gp_Pnt(0.0, 0.0, probeZ), gp_Dir(0.0, 0.0, 1.0));
        BRepAlgoAPI_Section section(face11->occtFace, plane, Standard_False);
        section.ComputePCurveOn1(Standard_False);
        section.Approximation(Standard_True);
        if (fuzzy > 0.0)
            section.SetFuzzyValue(fuzzy);
        section.Build();
        std::printf("\nDirect BRepAlgoAPI_Section on face 11 at z=%.4f fuzzy=%.6f: IsDone=%d\n", probeZ, fuzzy,
                    section.IsDone());
        if (section.IsDone())
        {
            int edgeIdx = 0;
            for (TopExp_Explorer exp(section.Shape(), TopAbs_EDGE); exp.More(); exp.Next(), edgeIdx++)
            {
                const TopoDS_Edge edge = TopoDS::Edge(exp.Current());
                std::vector<glm::dvec3> pts;
                GeometryOps::CollectPointsFromEdge(edge, 0.05, pts);
                std::printf("  section edge %d: %zu points, first=(%.6f,%.6f) last=(%.6f,%.6f)\n", edgeIdx,
                            pts.size(), pts.empty() ? 0 : pts.front().x, pts.empty() ? 0 : pts.front().y,
                            pts.empty() ? 0 : pts.back().x, pts.empty() ? 0 : pts.back().y);
            }
        }
    }

    std::vector<SliceLoop> probeLoops = Slice::ExtractLoops(probeSegs);
    std::printf("\nExtracted loops at z=%.4f (%zu total):\n", probeZ, probeLoops.size());
    for (size_t li = 0; li < probeLoops.size(); li++)
    {
        const SliceLoop &loop = probeLoops[li];
        std::printf("  loop %zu: isHole=%d ringPts=%zu\n", li, loop.isHole, loop.ring.size());
        for (const auto &p : loop.ring)
            std::printf("    (%.3f,%.3f,%.3f)\n", p.x, p.y, p.z);
    }

    return 0;
}
