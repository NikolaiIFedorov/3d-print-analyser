#pragma once

/// One-at-a-time geometry / import probes (rebuild between toggles).
namespace GeometryExperiments
{
// --- Theory: coplanar merge creates large single faces that z-fight internally or with neighbors.
// Set `true`, rebuild, re-import STL — if the artifact changes, merge is implicated.
inline constexpr bool kSkipStlMergeCoplanarFaces = false;
} // namespace GeometryExperiments
