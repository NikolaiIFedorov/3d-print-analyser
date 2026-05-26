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
    /// Chord tolerance for polylining arc / NURBS edges and fillet arcs, in millimetres. Tighter
    /// than print-resolution because fillet arcs are small (~2 mm radius) and benefit from the
    /// extra samples — at 0.02 mm a 2 mm-radius fillet hits ~6 segments per quarter-arc, which
    /// the fillet helper then floors at ≥ 8 for visual smoothness across radii.
    double chordTolMm = 0.02;
    /// Minimum feature size: small-hole strip suppression + small-face eligibility.
    double minFeatureMm = 1.5;
};

/// Builds preview line segments (in world space, on the face plane) for the **carved** pattern
/// only (inset minus strip, with corner fillets). Does not trace the face's outer boundary — the
/// Structure tool's face tint shows which faces are included. Returns empty if the face is null,
/// the outline is degenerate, CGAL is disabled, projection fails, or inset yields no geometry.
///
/// Debugging: set environment variable `CAD_DEBUG_STRUCTURE=1` to print strip/carve diagnostics
/// to stderr when a face is baked (cache miss), including vertex-count guard and chord selection.
///
/// Callers (currently `Display::RefreshStructurePreviewForRenderer`) accumulate segments across all
/// eligible faces that are **not** in the user's exclusion set and hand the union to
/// `SceneRenderer::SetStructurePreviewSegments`. Staging carve uses the same exclusions via
/// `Scene::Clone` face remapping (`Display::BeginStructureStagingSession`).
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

#include <functional>
/// Filleted outer boundaries of each carved `inset \ strip` region in **world** coordinates (3D
/// points on the face plane). Empty when geometry fails. Used by Structure carve commit (Phase C).
/// Optional `workerTrace`: forwarded from `TryApplyStructureCarve` for `structure_staging_worker_carve_phase`
/// tokens inside the footprint pipeline (`fp_outline_*`, `fp_collect_*`, `fp_2d_boolean_*`, …).
std::vector<std::vector<glm::dvec3>> BuildCarveFootprintOuterRingsWorld(
    const Face *face,
    const BakeParams &params,
    const std::function<void(const std::string &)> *workerTrace = nullptr);

} // namespace StructureTriangulation
