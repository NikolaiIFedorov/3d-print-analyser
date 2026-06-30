#pragma once

#include <unordered_set>
struct Solid;

#include <vector>
#include <TopoDS_Face.hxx>
#include "Edge.hpp"
struct Edge;

#include "OrientedEdge.hpp"

#include "Surface.hpp"

#include "Point.hpp"
struct Point;

class Face
{
public:
    Solid *dependency;

    std::vector<std::vector<OrientedEdge>> loops;
    std::unique_ptr<Surface> surface;
    TopoDS_Face occtFace;

    Face(std::vector<std::vector<Edge *>> edgePtrs);
    Face(std::vector<std::vector<Edge *>> edgePtrs, std::unique_ptr<Surface> surf);

    const Surface &GetSurface() const { return *surface; }

    /// True when the face still has loops and a surface (not tombstoned after OCCT rebuild).
    bool HasGeometry() const noexcept { return surface != nullptr && !loops.empty(); }

    /// Reverses every boundary loop (reverse edge order + toggle `reversed`) and recomputes
    /// `PlanarSurface` data. No-op if not planar. Used to fix same-directed shared edges between
    /// two manifold-adjacent faces (clean case).
    bool FlipWindingIfPlanar();

    /// Detaches from solid and drops renderable OCCT data. Orphaned faces stay in `Scene::faces`
    /// for pointer stability but must not keep drawing stale `occtFace` meshes in the loose pass.
    void ClearForRemoval() noexcept;

private:
    void OrientEdgeLoops(const std::vector<std::vector<Edge *>> &edgePtrs);
    PlanarData CalculatePlanarData();
};
