#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <TopoDS_Shape.hxx>

class Face;

namespace StructureTriangulation
{

/// Active Structure algorithm stage. While `CutOutline`, the tool builds and previews the trimmed
/// inset footprint only; strip, fillet, triangulation, and staging carve stay off.
enum class StructureDevelopmentPhase : uint8_t
{
    CutOutline,
    Full
};

/// Set to `Full` to re-enable strip/fillet preview and staging carve.
constexpr StructureDevelopmentPhase kStructureDevelopmentPhase = StructureDevelopmentPhase::Full;

constexpr bool StructureStopsAtCutOutline() noexcept
{
    return kStructureDevelopmentPhase == StructureDevelopmentPhase::CutOutline;
}

/// Structure preview overlay: face-plane trim vs XY carve footprint (project → offset in XY → boolean → map to face).
enum class StructurePreviewOutlineMode : uint8_t
{
    FacePlaneCutOutline,
    XyCarveFootprint
};

/// Rebuild after changing. `XyCarveFootprint` uses the same XY offset + boolean trim as vertical-prism carve.
constexpr StructurePreviewOutlineMode kStructurePreviewOutlineMode =
    StructurePreviewOutlineMode::XyCarveFootprint;

/// With `XyCarveFootprint`: false = draw trimmed footprint on the cap (tilted with the face); true = keep it in a
/// world-horizontal plane at the top of the cap (parallel to the grid / build XY).
constexpr bool kStructurePreviewDrawCarveFootprintInWorldXy = true;

constexpr bool StructurePreviewUsesXyCarveFootprint() noexcept
{
    return kStructurePreviewOutlineMode == StructurePreviewOutlineMode::XyCarveFootprint;
}

constexpr bool StructurePreviewDrawsCarveFootprintInWorldXy() noexcept
{
    return StructurePreviewUsesXyCarveFootprint() && kStructurePreviewDrawCarveFootprintInWorldXy;
}

/// Minimum outward-normal Z component (world +Z) for Structure eligibility and carve. Faces more
/// vertical than this are rejected because the XY footprint projection collapses.
inline constexpr double kMinUpComponent = 0.3;

/// Minimum face span in millimetres (3D AABB and XY projection) for Structure eligibility.
inline constexpr double kMinFaceSpanMm = 1.5;

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
    /// When true, struts are notched in as usual (untouched — this never drops a strut or its
    /// support), but each resulting disjoint void pocket of the notched carve footprint is then
    /// tested on its own: if the wall material needed to skin that pocket's boundary
    /// (perimeter * wallThicknessMm) would outweigh the pocket's own area, that pocket is filled
    /// back to solid instead of carved away. See `NotchStrutsIntoCurvedFootprint`. Deliberately
    /// ignores infill density: actual local density varies by slicer profile and position in the
    /// model and isn't something this tool can predict, so the test only compares each pocket's
    /// own area against its own wall-skin cost.
    bool weightAwareStrutFilter = true;
    /// Printed wall band width (mm) — roughly wall_count * line_width from the target slicer
    /// profile. Only used when `weightAwareStrutFilter` is true.
    double wallThicknessMm = 0.8;
};

/// Raw offset rings on the face plane (outer inset + one outset loop per hole).
std::vector<std::vector<glm::dvec3>> BuildFacePlaneOffsetPreviewRings(const Face *face,
                                                                      const BakeParams &params);

/// Cut outline on the face plane: outer inset minus hole outsets (OCCT boolean, MakeFace fallback).
std::vector<std::vector<glm::dvec3>> BuildTrimmedFacePlanePreviewRings(const Face *face,
                                                                       const BakeParams &params);

/// Closed edge-loop preview of the cut outline. Cached per `BakeParams` + `Face *`.
/// When `StructureStopsAtCutOutline()`, draws only trimmed rings (no raw-offset fallback).
std::vector<std::pair<glm::vec3, glm::vec3>> BuildCutOutlinePreviewLines(const Face *face,
                                                                         const BakeParams &params);

struct StrutAnchor
{
    glm::vec3 pos; ///< world-XY position at preview z (wall run's arc-length midpoint)
    glm::vec2 N;   ///< inward wall normal (unit, 2D)
};

/// One anchor per inset-footprint edge: position at edge midpoint + inward wall normal.
std::vector<StrutAnchor> BuildStrutAnchorPreviewPoints(const Face *face,
                                                       const BakeParams &params);

/// One brace per outline edge, anchored at that edge's midpoint: normally a symmetric pair (2
/// segments) chosen by jointly scoring every valid pair of struts leaving the anchor on
/// closeness to 45° from the walls each strut touches, combined length, and how well the
/// pair's bisector aligns with the edge's inward normal. Tight corners that admit only one
/// valid direction (e.g. small fillet arcs squeezed against a hole) get a single unpaired
/// strut instead of nothing. A strut is valid only if it stays entirely within the carve
/// region: it must not cross any ring boundary (outer wall or hole), and its midpoint must
/// land inside the outer ring.
std::vector<std::pair<glm::vec3, glm::vec3>> BuildStrutPreviewLines(const Face *face,
                                                                    const BakeParams &params);

/// Notched cut outline without ChFi2d fillets: same as `BuildCutOutlinePreviewLines` stage 0 but
/// with `NotchStrutsIntoCurvedFootprint` applied and `FilletCurvedFootprint` omitted — shows
/// rectangular corners at strut connections (debug stage 1).
std::vector<std::pair<glm::vec3, glm::vec3>> BuildNotchedCutOutlinePreviewLines(const Face *face,
                                                                                const BakeParams &params);

/// Unclipped mitered quad outlines for the selected struts — same corner computation as the real
/// strut builder but without the BRepAlgoAPI_Common footprint clip. Shows endpoints, miter angles,
/// and full quad extent before trimming (debug stage 4).
std::vector<std::pair<glm::vec3, glm::vec3>> BuildStrutQuadPreviewLines(const Face *face,
                                                                         const BakeParams &params);

/// All geometrically valid strut centerlines before best-pair selection — every (i,j) anchor pair
/// that passes `StrutIsValid` and the hole-strut length filter, drawn as raw centerline segments
/// (debug stage 2). Coordinates match the world-XY flat-projection frame of `BuildStrutPreviewLines`.
std::vector<std::pair<glm::vec3, glm::vec3>> BuildAllStrutCandidatePreviewLines(const Face *face,
                                                                                const BakeParams &params);

/// Each strut's rectangle — centerline offset by `±insetMm/2` perpendicular to the strut direction
/// (matching the `strutHalfWidth = insetMm * 0.5` convention already used by `ApplyStrutsToFootprint`),
/// then squared off against the walls its two ends connect to: each centerline endpoint sits exactly
/// on a `BuildStrutAnchorPreviewPoints` anchor, which now also carries that wall run's discretized
/// polyline, so each rail endpoint slides along the rail's own slope (within a bounded distance)
/// until it lands on *that specific wall's real geometry* — never an infinite extension of it, which
/// would loop back around for a curved wall and shoot the rail off in the wrong direction. Struts
/// where any endpoint can't be landed this way are dropped whole. Surviving strut quads are fused
/// into one shape and re-extracted as boundary rings, merging overlapping/crossing struts into
/// continuous outlines (matching the offset-footprint treatment).
std::vector<std::pair<glm::vec3, glm::vec3>> BuildStrutRailPreviewLines(const Face *face,
                                                                        const BakeParams &params);

void AppendPreviewLineSegments(const std::vector<std::pair<glm::vec3, glm::vec3>> &src,
                               std::vector<std::pair<glm::vec3, glm::vec3>> &dst);

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

/// Shared OCCT inset/outset footprint on the face plane (empty/null shape on failure).
TopoDS_Shape BuildOffsetFootprintOnFace(const Face *face, const BakeParams &params);

/// Whether the curve-native carve/preview path is active. Default ON; set env CAD_STRUCTURE_CURVED=0
/// to force the legacy polygon path. The curve path falls back to polygon on any per-face failure.
bool StructureCurvedCarveEnabled();

/// MVP curve-preserving carve footprint: inset outer + outset holes as a real OCCT face with its
/// analytic arc/ellipse/line edges intact (no discretization), holes cut via the explicit-plane
/// boolean path. SKIPS struts/fillets. Null on any failure (caller falls back to the polygon path).
TopoDS_Shape BuildCurvedCarveFootprintMvp(const Face *face, const BakeParams &params);

/// Notches the fused strut quads out of a curve-walled carve footprint (change B), keeping its
/// analytic edges. Returns the footprint unchanged when there are no struts, and falls back to the
/// un-notched footprint (never null) if the boolean fails/invalidates.
TopoDS_Shape NotchStrutsIntoCurvedFootprint(const Face *face, const BakeParams &params,
                                            const TopoDS_Shape &curvedFootprint);

/// Extrudes a curved (horizontal, z≈0) carve footprint straight up through [zBottom, zTop], keeping
/// curved walls. Validates the prism topology; null on failure.
TopoDS_Shape ExtrudeCurvedFootprintForCarve(const TopoDS_Shape &footprint, double zBottom, double zTop);

/// The exact footprint the cut-outline preview displays — base inset/trim outline with the fused
/// strut quads notched out and every corner rounded to `insetMm` — as a flat OCCT face ready for
/// vertical extrusion. Shares its ring pipeline with `BuildCutOutlinePreviewLines` (same base rings,
/// same `ApplyStrutCutoutsToOutlineRings` notch+fillet pass), so what Structure carve cuts always
/// matches what the preview shows. Null on failure.
TopoDS_Shape BuildNotchedCarveFootprint(const Face *face, const BakeParams &params);

/// All boundary rings of an offset footprint in XY (outer inset + outset hole loops). Drops only
/// sub‑0.25 mm slivers — hole loops are never removed at `minFeatureMm`. Used for preview lines
/// and carve extrusion input.
std::vector<std::vector<glm::dvec3>> ExtractOffsetRingsFromFootprint(const TopoDS_Shape &footprint,
                                                                     double chordTolMm,
                                                                     double zPlane = 0.0);

/// Discretizes the footprint into XY rings (wire order preserved) for preview / diagnostics.
/// When `minSpanMm > 0`, rings narrower than that span are dropped (legacy filter; prefer
/// `ExtractOffsetRingsFromFootprint` for Structure offset preview).
std::vector<std::vector<glm::dvec3>> ExtractPreviewRingsFromFootprint(const TopoDS_Shape &footprint,
                                                                       double chordTolMm,
                                                                       double minSpanMm);

/// Projects a face-plane footprint to world XY at `zBottom` and extrudes vertically through `[zBottom, zTop]`.
TopoDS_Shape ExtrudeFlattenedFootprint(const TopoDS_Shape &footprintOnFacePlane, double zBottom, double zTop,
                                       double chordTolMm, double minSpanMm,
                                       const glm::dvec3 &normalHint = glm::dvec3(0.0, 0.0, 1.0));

} // namespace StructureTriangulation
