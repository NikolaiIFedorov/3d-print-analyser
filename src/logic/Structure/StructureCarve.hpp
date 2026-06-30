#pragma once

#include "Import/ImportProgress.hpp"

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
/// `MergeCoplanarFaces`. Faces must belong to `solid` and have normal Z ≥ `kMinUpComponent` so the
/// Carve prisms extrude vertically: near-horizontal faces through the full solid Z slab; slanted
/// faces from `min(face Z)` through the solid top (see structure_face_triangulation plan).
/// On failure returns `false` and leaves `solid` unchanged **for paths that return before**
/// `DetachFacesFromSolid` (normal errors / caught exceptions).
///
/// Each face must have at least one boundary loop (outer plus optional hole loops).
///
/// Optional `shouldAbort`: when non-null and returns true, aborts cooperatively between faces.
/// On abort returns `false` and sets `*errOut` when `errOut` is non-null.
///
/// Optional `workerTrace`: when non-null and the function is non-empty, invoked with short phase
/// tokens (`enter`, `after_z_bounds`, `before_prism`, `after_prism`, …).
///
/// Optional `progress`: when non-null and the function is non-empty, invoked with a human-readable
/// phase label and a 0..1 fraction covering the whole call (per-face prism/boolean-cut progress,
/// then the post-loop unify/rebuild stages), the same checkpoint style `STLImport`/`OBJImport` use.
///
/// `keepFaces`: other eligible faces of `solid` deliberately excluded from this carve (untouched by
/// any cut). Without these, the post-cut `ShapeUpgrade_UnifySameDomain` pass can merge an excluded
/// face's untouched geometry into a coplanar carved neighbor's leftover fragment, shifting its
/// centroid enough that the renderer can no longer match the carved result back to the original
/// face it was excluded from (toggling it back in then silently fails). Passed to
/// `ShapeUpgrade_UnifySameDomain::KeepShape` so excluded faces keep their own boundary.
bool TryApplyStructureCarve(Scene *scene,
                            Solid *solid,
                            const std::vector<const Face *> &faces,
                            const StructureTriangulation::BakeParams &params,
                            std::string *errOut,
                            const std::function<bool()> *shouldAbort = nullptr,
                            const std::function<void(const std::string &)> *workerTrace = nullptr,
                            const ImportProgressCallback *progress = nullptr,
                            const std::vector<const Face *> &keepFaces = {});

} // namespace StructureCarve
