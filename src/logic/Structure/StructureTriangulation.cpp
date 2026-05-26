#include "Structure/StructureTriangulation.hpp"
#include "Geometry/Face.hpp"
#include "Geometry/OrientedEdge.hpp"
#include "Geometry/Edge.hpp"
#include "Geometry/Point.hpp"
#include "Geometry/Surface.hpp"

#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Tool.hxx>
#include <BOPAlgo_BuilderFace.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopExp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <Poly_Polygon3D.hxx>
#include <gp_Pnt.hxx>
#include <TColgp_Array1OfPnt.hxx>

#include <unordered_map>

namespace StructureTriangulation
{

struct CacheKey
{
    const Face *face;
    double insetMm;
    double chordTolMm;
    double minFeatureMm;
    bool operator==(const CacheKey &other) const noexcept
    {
        return face == other.face && insetMm == other.insetMm && chordTolMm == other.chordTolMm &&
               minFeatureMm == other.minFeatureMm;
    }
};

struct CacheKeyHash
{
    std::size_t operator()(const CacheKey &k) const noexcept
    {
        std::size_t h = std::hash<const Face *>{}(k.face);
        h ^= std::hash<long long>{}(static_cast<long long>(k.insetMm * 1e6)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<long long>{}(static_cast<long long>(k.chordTolMm * 1e6)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<long long>{}(static_cast<long long>(k.minFeatureMm * 1e6)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

static std::unordered_map<CacheKey, std::vector<std::pair<glm::vec3, glm::vec3>>, CacheKeyHash>& BakeCache()
{
    static std::unordered_map<CacheKey, std::vector<std::pair<glm::vec3, glm::vec3>>, CacheKeyHash> cache;
    return cache;
}

static std::vector<std::vector<glm::dvec3>> BuildOffsetRings(const Face *face, const BakeParams &params)
{
    std::vector<std::vector<glm::dvec3>> rings;
    if (face == nullptr || face->loops.empty())
        return rings;

    std::unique_ptr<BRepBuilderAPI_MakeFace> mkFace;
    for (const auto &loop : face->loops)
    {
        BRepBuilderAPI_MakePolygon poly;
        for (const OrientedEdge &oe : loop)
        {
            if (oe.GetStart() != nullptr)
            {
                glm::dvec3 p = oe.GetStartPosition();
                poly.Add(gp_Pnt(p.x, p.y, p.z));
            }
        }
        poly.Close();
        if (poly.IsDone())
        {
            if (!mkFace)
            {
                mkFace = std::make_unique<BRepBuilderAPI_MakeFace>(poly.Wire());
            }
            else
            {
                mkFace->Add(poly.Wire());
            }
        }
    }

    if (!mkFace || !mkFace->IsDone())
        return rings;

    TopoDS_Face occtFace = mkFace->Face();

    if (params.insetMm <= 1e-6)
    {
        // If no inset, just return the original loops
        for (const auto &loop : face->loops)
        {
            std::vector<glm::dvec3> ring;
            for (const OrientedEdge &oe : loop)
            {
                if (oe.GetStart() != nullptr)
                    ring.push_back(oe.GetStartPosition());
            }
            if (!ring.empty())
                rings.push_back(ring);
        }
        return rings;
    }

    // Step 1: Inset by 2 * insetMm to suppress small features and create room for fillets
    BRepOffsetAPI_MakeOffset offsetMaker;
    offsetMaker.Init(occtFace, GeomAbs_Arc);
    offsetMaker.Perform(-2.0 * params.insetMm);
    if (!offsetMaker.IsDone())
        return rings;
    TopoDS_Shape innerShape = offsetMaker.Shape();

    TopTools_ListOfShape innerEdges;
    for (TopExp_Explorer ex(innerShape, TopAbs_EDGE); ex.More(); ex.Next()) {
        innerEdges.Append(ex.Current());
    }
    if (innerEdges.IsEmpty())
        return rings;

    BOPAlgo_BuilderFace innerBuilder;
    innerBuilder.SetFace(occtFace);
    innerBuilder.SetShapes(innerEdges);
    innerBuilder.Perform();
    const TopTools_ListOfShape& innerFaces = innerBuilder.Areas();

    // Step 2: Outset by insetMm to create the final filleted footprint
    TopoDS_Shape currentUnion;
    bool firstUnion = true;
    for (TopTools_ListIteratorOfListOfShape it(innerFaces); it.More(); it.Next()) {
        TopoDS_Face f = TopoDS::Face(it.Value());
        BRepOffsetAPI_MakeOffset offsetMaker2;
        offsetMaker2.Init(f, GeomAbs_Arc);
        offsetMaker2.Perform(params.insetMm);
        if (!offsetMaker2.IsDone()) continue;
        TopoDS_Shape outset = offsetMaker2.Shape();
        
        TopTools_ListOfShape outsetEdges;
        for (TopExp_Explorer ex(outset, TopAbs_EDGE); ex.More(); ex.Next()) {
            outsetEdges.Append(ex.Current());
        }
        if (outsetEdges.IsEmpty()) continue;

        BOPAlgo_BuilderFace outsetBuilder;
        outsetBuilder.SetFace(occtFace);
        outsetBuilder.SetShapes(outsetEdges);
        outsetBuilder.Perform();
        
        TopoDS_Shape outsetFace;
        if (outsetBuilder.Areas().Extent() > 0) {
            outsetFace = outsetBuilder.Areas().First();
        } else {
            continue;
        }
        
        if (firstUnion) {
            currentUnion = outsetFace;
            firstUnion = false;
        } else {
            BRepAlgoAPI_Fuse fuser(currentUnion, outsetFace);
            fuser.Build();
            if (fuser.IsDone()) {
                currentUnion = fuser.Shape();
            }
        }
    }

    if (firstUnion) return rings; // no valid outsets

    ShapeUpgrade_UnifySameDomain unifier(currentUnion, Standard_True, Standard_True, Standard_True);
    unifier.Build();
    currentUnion = unifier.Shape();

    // Step 3: Discretize the arcs into line segments
    BRepMesh_IncrementalMesh mesher(currentUnion, params.chordTolMm);

    // Step 4: Extract the polygons
    for (TopExp_Explorer exWire(currentUnion, TopAbs_WIRE); exWire.More(); exWire.Next())
    {
        TopoDS_Wire wire = TopoDS::Wire(exWire.Current());
        std::vector<glm::dvec3> ring;
        for (BRepTools_WireExplorer exEdge(wire); exEdge.More(); exEdge.Next())
        {
            TopoDS_Edge edge = exEdge.Current();
            TopLoc_Location loc;
            Handle(Poly_Polygon3D) poly3d = BRep_Tool::Polygon3D(edge, loc);
            if (!poly3d.IsNull())
            {
                const TColgp_Array1OfPnt& nodes = poly3d->Nodes();
                // Skip the last node of each edge because it's the first node of the next edge
                for (int i = nodes.Lower(); i < nodes.Upper(); ++i)
                {
                    gp_Pnt p = nodes.Value(i).Transformed(loc);
                    glm::dvec3 pt(p.X(), p.Y(), p.Z());
                    if (ring.empty() || glm::distance(ring.back(), pt) > 1e-6)
                        ring.push_back(pt);
                }
            }
            else
            {
                // Fallback to just the start vertex if no polygon
                TopoDS_Vertex v1, v2;
                TopExp::Vertices(edge, v1, v2);
                gp_Pnt p = BRep_Tool::Pnt(v1);
                glm::dvec3 pt(p.X(), p.Y(), p.Z());
                if (ring.empty() || glm::distance(ring.back(), pt) > 1e-6)
                    ring.push_back(pt);
            }
        }
        if (!ring.empty())
            rings.push_back(ring);
    }

    return rings;
}

std::vector<std::pair<glm::vec3, glm::vec3>> BuildFaceTriangulationPreview(const Face *face,
                                                                          const BakeParams &params)
{
    if (face == nullptr)
        return {};
    const CacheKey key{face, params.insetMm, params.chordTolMm, params.minFeatureMm};
    auto &cache = BakeCache();
    if (auto it = cache.find(key); it != cache.end())
        return it->second;

    std::vector<std::pair<glm::vec3, glm::vec3>> segments;
    std::vector<std::vector<glm::dvec3>> rings = BuildOffsetRings(face, params);
    
    for (const auto &ring : rings)
    {
        if (ring.size() < 3) continue;
        for (size_t i = 0; i < ring.size(); ++i)
        {
            size_t j = (i + 1) % ring.size();
            segments.push_back({glm::vec3(ring[i].x, ring[i].y, ring[i].z),
                                glm::vec3(ring[j].x, ring[j].y, ring[j].z)});
        }
    }

    cache.emplace(key, segments);
    return segments;
}

void ClearBakeCache()
{
    BakeCache().clear();
}

void InvalidateBakeCacheForParams(const BakeParams &params)
{
    auto &cache = BakeCache();
    for (auto it = cache.begin(); it != cache.end();)
    {
        if (it->first.insetMm != params.insetMm || it->first.chordTolMm != params.chordTolMm ||
            it->first.minFeatureMm != params.minFeatureMm)
            it = cache.erase(it);
        else
            ++it;
    }
}

std::vector<std::vector<glm::dvec3>> BuildCarveFootprintOuterRingsWorld(
    const Face *face,
    const BakeParams &params,
    const std::function<void(const std::string &)> *workerTrace)
{
    return BuildOffsetRings(face, params);
}

} // namespace StructureTriangulation
