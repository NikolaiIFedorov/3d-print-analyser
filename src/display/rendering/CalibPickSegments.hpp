#pragma once

#include "ScenePick.hpp"

class Scene;

namespace CalibPickSegments
{

/// Fills `out` with straight and tessellated segments for face boundaries and standalone edges
/// (same coverage as wireframe picking geometry).
void Build(const Scene *scene, std::vector<PickSegment> &out);

} // namespace CalibPickSegments
