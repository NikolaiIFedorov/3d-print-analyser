#pragma once

#include <string>
#include <vector>

class Scene;
struct Solid;
struct Face;

namespace StructureTriangulation
{
struct BakeParams;
}

namespace StructureCarve
{

/// Subtracts vertical prisms from `solid` for each `Face` in `faces` (Structure footprint:
/// filleted `inset \ strip` regions), then rebuilds scene topology from the CGAL mesh and runs
/// `MergeCoplanarFaces`. Faces must belong to `solid` and be near-horizontal (|n·ẑ| ≥ 0.995) for
/// the current prism extruder. On failure returns `false` and leaves `solid` unchanged.
///
/// Requires `CAD_USE_CGAL` and a triangulated solid (STL-style triangle soup).
#if defined(CAD_USE_CGAL)
bool TryApplyStructureCarve(Scene *scene,
                            Solid *solid,
                            const std::vector<const Face *> &faces,
                            const StructureTriangulation::BakeParams &params,
                            std::string *errOut);
#endif

} // namespace StructureCarve
