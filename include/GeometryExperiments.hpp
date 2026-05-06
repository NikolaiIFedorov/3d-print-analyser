#pragma once

#include <cstdint>

/// One-at-a-time geometry / import probes (rebuild between toggles).
namespace GeometryExperiments
{
/// Which sources populate the Calibrate hole-loop edge set in `RebuildHoleCalibTopology`. Classification is
/// always “face touches any hole-loop edge”; this controls **which** edges are in that set. Rebuild and re-import
/// to test. `All` is production behavior.
enum class CalibHoleQualifyProbe : std::uint8_t
{
    All = 0,
    /// Inner-loop scan on multi-loop stack-parallel caps only (no tessellated plane-boundary tracing).
    MultiLoopCapOnly,
    /// Tessellated cap rim trace only (no B-rep inner-loop scan).
    TessellatedRimCapOnly,
};

inline constexpr CalibHoleQualifyProbe kCalibHoleQualifyProbe = CalibHoleQualifyProbe::All;

// --- Theory: coplanar merge creates large single faces that z-fight internally or with neighbors.
// Set `true`, rebuild, re-import STL — if the artifact changes, merge is implicated.
inline constexpr bool kSkipStlMergeCoplanarFaces = false;

// Coplanar merge accepts a pair when |dot(ni, nj)| > 1 - kMergeCoplanarNormalDotSlack (opposite
// winding on the same flat counts). Tighter rejects more sliver noise; looser risks curved merges.
// Default 5e-3; raise only if flats still tessellate after rebuild + re-import.
inline constexpr double kMergeCoplanarNormalDotSlack = 5e-3;

// Minimum plane-distance tolerance for accepting adjacent near-coplanar STL facets. For mm-based
// print CAD, 0.001 mm is intended to catch exporter / float drift without hiding printable detail.
inline constexpr double kMergeCoplanarPlaneTolFloor = 1e-3;

#ifdef CAD_CGAL_PLANAR_REMESH_EXPERIMENT_ENABLED
// Requires CMake `-DCAD_EXPERIMENTAL_CGAL_PLANAR_REMESH=ON` and CGAL on the toolchain.
// Default `false`: STL import uses homegrown `MergeCoplanarFaces` only (best wireframe / solid
// quality in practice). Set `true` to A/B CGAL `remesh_planar_patches` first (soup rebuild),
// then the same merge pass; on CGAL failure, import falls back to merge-only.
inline constexpr bool kUseCgalRemeshPlanarPatchesForStl = false;
#endif
} // namespace GeometryExperiments
