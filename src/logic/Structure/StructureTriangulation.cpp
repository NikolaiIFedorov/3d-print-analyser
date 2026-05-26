#include "Structure/StructureTriangulation.hpp"
#include "Geometry/Face.hpp"
#include "Geometry/OrientedEdge.hpp"
#include "Geometry/Edge.hpp"
#include "Geometry/Point.hpp"
#include "Geometry/Surface.hpp"

#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopExp.hxx>
#include <gp_Pnt.hxx>

namespace StructureTriangulation
{

std::vector<std::pair<glm::vec3, glm::vec3>> BuildFaceTriangulationPreview(const Face *face,
                                                                          const BakeParams &params)
{
    std::vector<std::pair<glm::vec3, glm::vec3>> segments;
    if (face == nullptr || face->loops.empty())
        return segments;

    BRepBuilderAPI_MakePolygon poly;
    for (const OrientedEdge &oe : face->loops[0])
    {
        if (oe.GetStart() != nullptr)
        {
            glm::dvec3 p = oe.GetStartPosition();
            poly.Add(gp_Pnt(p.x, p.y, p.z));
        }
    }
    poly.Close();

    if (!poly.IsDone())
        return segments;

    BRepOffsetAPI_MakeOffset offsetMaker;
    offsetMaker.Init(GeomAbs_Intersection);
    offsetMaker.AddWire(poly.Wire());
    offsetMaker.Perform(-params.insetMm);

    TopoDS_Shape result = offsetMaker.Shape();
    for (TopExp_Explorer ex(result, TopAbs_EDGE); ex.More(); ex.Next())
    {
        TopoDS_Edge edge = TopoDS::Edge(ex.Current());
        TopoDS_Vertex v1, v2;
        TopExp::Vertices(edge, v1, v2);
        gp_Pnt p1 = BRep_Tool::Pnt(v1);
        gp_Pnt p2 = BRep_Tool::Pnt(v2);
        segments.push_back({glm::vec3(p1.X(), p1.Y(), p1.Z()), glm::vec3(p2.X(), p2.Y(), p2.Z())});
    }

    return segments;
}

void ClearBakeCache()
{
}

void InvalidateBakeCacheForParams(const BakeParams &params)
{
}

std::vector<std::vector<glm::dvec3>> BuildCarveFootprintOuterRingsWorld(
    const Face *face,
    const BakeParams &params,
    const std::function<void(const std::string &)> *workerTrace)
{
    std::vector<std::vector<glm::dvec3>> rings;
    if (face == nullptr || face->loops.empty())
        return rings;

    BRepBuilderAPI_MakePolygon poly;
    for (const OrientedEdge &oe : face->loops[0])
    {
        if (oe.GetStart() != nullptr)
        {
            glm::dvec3 p = oe.GetStartPosition();
            poly.Add(gp_Pnt(p.x, p.y, p.z));
        }
    }
    poly.Close();

    if (!poly.IsDone())
        return rings;

    BRepOffsetAPI_MakeOffset offsetMaker;
    offsetMaker.Init(GeomAbs_Intersection);
    offsetMaker.AddWire(poly.Wire());
    offsetMaker.Perform(-params.insetMm);

    TopoDS_Shape result = offsetMaker.Shape();
    for (TopExp_Explorer exWire(result, TopAbs_WIRE); exWire.More(); exWire.Next())
    {
        TopoDS_Wire wire = TopoDS::Wire(exWire.Current());
        std::vector<glm::dvec3> ring;
        for (BRepTools_WireExplorer exEdge(wire); exEdge.More(); exEdge.Next())
        {
            TopoDS_Edge edge = exEdge.Current();
            TopoDS_Vertex v1, v2;
            TopExp::Vertices(edge, v1, v2);
            gp_Pnt p1 = BRep_Tool::Pnt(v1);
            if (ring.empty() || glm::distance(ring.back(), glm::dvec3(p1.X(), p1.Y(), p1.Z())) > 1e-6)
                ring.push_back(glm::dvec3(p1.X(), p1.Y(), p1.Z()));
        }
        if (!ring.empty())
            rings.push_back(ring);
    }

    return rings;
}

} // namespace StructureTriangulation
