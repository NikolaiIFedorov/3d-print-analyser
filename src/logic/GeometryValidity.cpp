#include "GeometryValidity.hpp"

#include "Geometry/Edge.hpp"
#include "Geometry/Face.hpp"
#include "Geometry/OrientedEdge.hpp"
#include "Geometry/Point.hpp"
#include "Geometry/Solid.hpp"
#include "Geometry/Surface.hpp"
#include "scene/scene.hpp"
#include "utils/log.hpp"

#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_Result.hxx>
#include <BRepCheck_Status.hxx>
#include <ShapeFix_Shape.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRep_Builder.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace GeometryValidity
{
OpenBoundaryDebugStats CollectOpenBoundaryDebugStatsForSolid(const Solid &solid) noexcept
{
    OpenBoundaryDebugStats stats{};
    return stats;
}

AppInvalidTag EvaluateAppInvalidTagsForSolid(const Solid &solid) noexcept
{
    AppInvalidTag tags = AppInvalidTag::None;

    if (solid.occtShape.IsNull())
    {
        tags |= AppInvalidTag::NullOrEmptyTopology;
        return tags;
    }

    BRepCheck_Analyzer analyzer(solid.occtShape, Standard_True);
    if (analyzer.IsValid())
        return tags;

    for (TopExp_Explorer ex(solid.occtShape, TopAbs_FACE); ex.More(); ex.Next())
    {
        const TopoDS_Shape& face = ex.Current();
        if (!analyzer.Result(face).IsNull())
        {
            for (auto it = analyzer.Result(face)->Status().cbegin(); it != analyzer.Result(face)->Status().cend(); ++it)
            {
                BRepCheck_Status status = *it;
                if (status == BRepCheck_NotClosed)
                    tags |= AppInvalidTag::OpenBoundary;
                else if (status == BRepCheck_SelfIntersectingWire || status == BRepCheck_IntersectingWires)
                    tags |= AppInvalidTag::SelfIntersection;
                else if (status == BRepCheck_InvalidMultiConnexity)
                    tags |= AppInvalidTag::NonManifoldConnectivity;
                else if (status == BRepCheck_BadOrientation || status == BRepCheck_BadOrientationOfSubshape)
                    tags |= AppInvalidTag::InconsistentFaceOrientation;
                else if (status == BRepCheck_InvalidDegeneratedFlag || status == BRepCheck_RedundantEdge || status == BRepCheck_RedundantFace)
                    tags |= AppInvalidTag::DegenerateTriangle;
            }
        }
    }

    for (TopExp_Explorer ex(solid.occtShape, TopAbs_EDGE); ex.More(); ex.Next())
    {
        const TopoDS_Shape& edge = ex.Current();
        if (!analyzer.Result(edge).IsNull())
        {
            for (auto it = analyzer.Result(edge)->Status().cbegin(); it != analyzer.Result(edge)->Status().cend(); ++it)
            {
                BRepCheck_Status status = *it;
                if (status == BRepCheck_InvalidMultiConnexity)
                    tags |= AppInvalidTag::NonManifoldConnectivity;
                else if (status == BRepCheck_InvalidDegeneratedFlag || status == BRepCheck_RedundantEdge)
                    tags |= AppInvalidTag::DegenerateTriangle;
            }
        }
    }

    return tags;
}

void CollectOpenBoundaryEdgesForSolid(const Solid &solid, std::vector<const Edge *> &out) noexcept
{
    out.clear();
    if (solid.occtShape.IsNull())
        return;

    TopTools_IndexedDataMapOfShapeListOfShape edgeFaceMap;
    TopExp::MapShapesAndAncestors(solid.occtShape, TopAbs_EDGE, TopAbs_FACE, edgeFaceMap);

    for (int i = 1; i <= edgeFaceMap.Extent(); ++i)
    {
        const TopoDS_Edge& edge = TopoDS::Edge(edgeFaceMap.FindKey(i));
        const TopTools_ListOfShape& faces = edgeFaceMap.FindFromIndex(i);

        if (faces.Extent() == 1)
        {
            // Find corresponding CAD_OpenGL Edge
            for (Face *face : solid.faces)
            {
                if (face == nullptr) continue;
                for (const auto &loop : face->loops)
                {
                    for (const OrientedEdge &oe : loop)
                    {
                        if (oe.edge != nullptr && !oe.edge->occtEdge.IsNull() && oe.edge->occtEdge.IsSame(edge))
                        {
                            out.push_back(oe.edge);
                        }
                    }
                }
            }
        }
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

std::string DescribeAppInvalidTagsForLog(AppInvalidTag flags)
{
    if (!Any(flags))
        return {};

    std::string out;
    auto append = [&out](const char *name)
    {
        if (!out.empty())
            out += ", ";
        out += name;
    };

    using U = std::underlying_type_t<AppInvalidTag>;
    const U raw = static_cast<U>(flags);
    if (raw & static_cast<U>(AppInvalidTag::DegenerateTriangle))
        append("DegenerateTriangle");
    if (raw & static_cast<U>(AppInvalidTag::NullOrEmptyTopology))
        append("NullOrEmptyTopology");
    if (raw & static_cast<U>(AppInvalidTag::SelfIntersection))
        append("SelfIntersection");
    if (raw & static_cast<U>(AppInvalidTag::OpenBoundary))
        append("OpenBoundary");
    if (raw & static_cast<U>(AppInvalidTag::NonManifoldConnectivity))
        append("NonManifoldConnectivity");
    if (raw & static_cast<U>(AppInvalidTag::InconsistentFaceOrientation))
        append("InconsistentFaceOrientation");

    return out;
}

void RefreshSolidAppGeometryValidityCache(Solid &solid) noexcept
{
    solid.cachedAppInvalidGeometryTags = EvaluateAppInvalidTagsForSolid(solid);
    solid.cachedAppInvalidGeometryTagsFresh = true;
}

void InvalidateSolidAppGeometryValidityCache(Solid &solid) noexcept
{
    solid.cachedAppInvalidGeometryTags = AppInvalidTag::None;
    solid.cachedAppInvalidGeometryTagsFresh = false;
}

bool TryRepairSolidBRep(Scene *scene, Solid &solid) noexcept
{
    if (solid.occtShape.IsNull() || scene == nullptr)
        return false;

    TopoDS_Shape currentShape = solid.occtShape;
    bool modified = false;

    // 1. Fix self-intersections using BRepAlgoAPI_Fuse with an empty solid
    AppInvalidTag tags = EvaluateAppInvalidTagsForSolid(solid);
    if (Any(tags & AppInvalidTag::SelfIntersection))
    {
        TopoDS_Solid emptySolid;
        BRep_Builder B;
        B.MakeSolid(emptySolid);

        BRepAlgoAPI_Fuse fuser(currentShape, emptySolid);
        fuser.Build();
        if (!fuser.HasErrors())
        {
            currentShape = fuser.Shape();
            modified = true;
            LOG_BACK("OCCT BRepAlgoAPI_Fuse applied self-intersection repairs to solid.");
        }
    }

    // 2. Fix other topological issues using ShapeFix_Shape
    ShapeFix_Shape fixer(currentShape);
    fixer.Perform();
    
    TopoDS_Shape fixedShape = fixer.Shape();
    if (!fixedShape.IsSame(currentShape))
    {
        currentShape = fixedShape;
        modified = true;
        LOG_BACK("OCCT ShapeFix_Shape applied repairs to solid.");
    }

    if (!modified)
        return false;

    // Shape was modified, we need to rebuild the CAD_OpenGL structs
    for (Face *face : solid.faces)
    {
        if (face == nullptr) continue;
        for (auto &loop : face->loops)
        {
            for (auto &oe : loop)
            {
                if (oe.edge != nullptr)
                    oe.edge->dependencies.erase(face);
            }
        }
        face->loops.clear();
        face->dependency = nullptr;
    }
    solid.faces.clear();

    scene->PopulateSolidFromOcctShape(&solid, currentShape);
    
    return true;
}

} // namespace GeometryValidity
