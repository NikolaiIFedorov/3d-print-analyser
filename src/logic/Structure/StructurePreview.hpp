#pragma once

#include <utility>
#include <vector>

#include <glm/glm.hpp>

class Scene;

namespace StructurePreview
{

/// Heuristic inward struts: each planar face of each solid gets a segment from face centroid
/// toward the solid's bounding-box center, with length clamped for a readable preview.
void BuildCenterStruts(const Scene &scene, std::vector<std::pair<glm::vec3, glm::vec3>> &out);

} // namespace StructurePreview
