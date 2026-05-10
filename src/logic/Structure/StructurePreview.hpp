#pragma once

#include <utility>
#include <vector>

#include <glm/glm.hpp>

class Scene;

namespace StructurePreview
{

/// Which internal preview graph to draw (line segments only until mesh export exists).
enum class PreviewPattern : int
{
    /// Edges of the face-adjacency graph: segment between centroids of planar faces that share an edge
    /// (e.g. cube → interior octahedron / “3D diamond”).
    AdjacentFaceMidpoints = 0,
    /// Legacy: short struts from each planar face centroid toward the solid bbox center.
    CenterStrutsBBox = 1,
};

/// Heuristic inward struts: each planar face of each solid gets a segment from face centroid
/// toward the solid's bounding-box center, with length clamped for a readable preview.
void BuildCenterStruts(const Scene &scene, std::vector<std::pair<glm::vec3, glm::vec3>> &out);

/// Dual-style wireframe inside the solid: for every manifold edge shared by exactly two planar faces
/// of the same solid, draw a segment between those face centroids.
void BuildAdjacentFaceMidpoints(const Scene &scene, std::vector<std::pair<glm::vec3, glm::vec3>> &out);

} // namespace StructurePreview
