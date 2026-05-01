#pragma once

/// One-at-a-time geometry / import probes (rebuild between toggles).
namespace GeometryExperiments
{
// --- Theory: coplanar merge creates large single faces that z-fight internally or with neighbors.
// Set `true`, rebuild, re-import STL — if the artifact changes, merge is implicated.
inline constexpr bool kSkipStlMergeCoplanarFaces = false;

// Coplanar merge accepts a pair when dot(ni, nj) > 1 - kMergeCoplanarNormalDotSlack.
// Tighter (e.g. 1e-3) rejects more sliver-triangle normal noise; looser (e.g. 1e-2) risks
// merging facets on gently curved regions. Tune per model / unit scale.
inline constexpr double kMergeCoplanarNormalDotSlack = 5e-3;

#ifdef CAD_CGAL_PLANAR_REMESH_EXPERIMENT_ENABLED
// Requires CMake `-DCAD_EXPERIMENTAL_CGAL_PLANAR_REMESH=ON` and CGAL on the toolchain.
// Default `false`: STL import uses homegrown `MergeCoplanarFaces` only (best wireframe / solid
// quality in practice). Set `true` to A/B CGAL `remesh_planar_patches` first (soup rebuild),
// then the same merge pass; on CGAL failure, import falls back to merge-only.
inline constexpr bool kUseCgalRemeshPlanarPatchesForStl = false;
#endif
} // namespace GeometryExperiments
