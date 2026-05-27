#pragma once

#include <functional>
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
/// filleted inset regions), then rebuilds scene topology from the OCCT shape and runs
/// `MergeCoplanarFaces`. Faces must belong to `solid` and be near-horizontal (|n·ẑ| ≥ 0.995) for
/// the current prism extruder. On failure returns `false` and leaves `solid` unchanged **for paths
/// that return before** `DetachFacesFromSolid` (normal errors / caught exceptions).
///
/// Each face must be a single **simple** outer loop (3+ edges).
///
/// Optional `shouldAbort`: when non-null and returns true, aborts cooperatively between faces.
/// On abort returns `false` and sets `*errOut` when `errOut` is non-null.
///
/// Optional `workerTrace`: when non-null and the function is non-empty, invoked with short phase
/// tokens (`enter`, `after_z_bounds`, `before_prism`, `after_prism`, …).
bool TryApplyStructureCarve(Scene *scene,
                            Solid *solid,
                            const std::vector<const Face *> &faces,
                            const StructureTriangulation::BakeParams &params,
                            std::string *errOut,
                            const std::function<bool()> *shouldAbort = nullptr,
                            const std::function<void(const std::string &)> *workerTrace = nullptr);

} // namespace StructureCarve
