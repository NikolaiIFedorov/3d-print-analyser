#pragma once

#include <string>
#include <vector>

#if defined(CAD_USE_CGAL)
#include <functional>
#endif

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
/// the current prism extruder. On failure returns `false` and leaves `solid` unchanged **for paths
/// that return before** `DetachFacesFromSolid` (normal errors / caught exceptions). A **hang or hard
/// abort inside CGAL** does not complete that contract until the call returns.
///
/// Requires `CAD_USE_CGAL`. Each face must be a single **simple** outer loop (3+ edges); n-gons
/// are fan-triangulated for the CGAL soup (convex facets typical after STL coplanar merge).
#if defined(CAD_USE_CGAL)
/// Optional `shouldAbort`: when non-null and returns true, aborts cooperatively between faces/rings
/// and **immediately before each** `corefine_and_compute_difference` call (not inside it).
/// CGAL `corefine` is not interruptible once started. On abort returns `false` and sets `*errOut`
/// when `errOut` is non-null.
///
/// Optional `workerTrace`: when non-null and the function is non-empty, invoked with short phase
/// tokens (`enter`, `tm_ready`, `after_z_bounds`, `before_footprint`, `footprint_done_rings_N`,
/// `before_prism`, `after_prism`, `before_boolean`, …). CGAL may print to **stderr** independently of
/// these events — stderr lines are not proof the call has returned to C++ yet.
bool TryApplyStructureCarve(Scene *scene,
                            Solid *solid,
                            const std::vector<const Face *> &faces,
                            const StructureTriangulation::BakeParams &params,
                            std::string *errOut,
                            const std::function<bool()> *shouldAbort = nullptr,
                            const std::function<void(const std::string &)> *workerTrace = nullptr);
#endif

} // namespace StructureCarve
