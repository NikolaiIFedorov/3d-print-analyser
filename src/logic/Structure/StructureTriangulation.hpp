#pragma once

#include <utility>
#include <vector>

#include <glm/glm.hpp>

class Face;

namespace StructureTriangulation
{

/// Per-face inputs that key the bake cache. Equal `BakeParams` for the same `Face *` returns cached
/// preview segments without re-running CGAL. Changing any field (in practice: `insetMm` via the
/// panel slider) invalidates the cached entry for that face.
struct BakeParams
{
    /// Inset distance in millimetres. Also drives strip-band width and fillet radius (1:1).
    double insetMm = 2.0;
    /// Chord tolerance for polylining arc / NURBS edges, in millimetres.
    double chordTolMm = 0.1;
    /// Minimum feature size: small-hole strip suppression + small-face eligibility.
    double minFeatureMm = 1.5;
};

/// Builds preview line segments (in world space, sitting on the face's actual plane) representing
/// the triangulation pattern for one face. Returns an empty vector if the face is null, ineligible,
/// or — at B2a — for any face at all (the algorithm is stubbed until B2b lands).
///
/// Callers (currently `Display::RefreshStructurePreviewForRenderer`) accumulate segments across all
/// non-excluded eligible faces and hand the union to `SceneRenderer::SetStructurePreviewSegments`.
std::vector<std::pair<glm::vec3, glm::vec3>> BuildFaceTriangulationPreview(const Face *face,
                                                                          const BakeParams &params);

/// Drops every entry in the per-face bake cache. Call when a scene reload invalidates `Face *`
/// pointers or when the user wants a forced rebuild.
void ClearBakeCache();

/// Drops cached entries whose `BakeParams` differ from `params`. Use when the panel slider changes
/// — every face needs a re-bake against the new inset value, but cache entries pinned to other
/// faces under unrelated params (e.g. different solids in a multi-solid scene with per-solid
/// parameters in the future) survive.
void InvalidateBakeCacheForParams(const BakeParams &params);

} // namespace StructureTriangulation
