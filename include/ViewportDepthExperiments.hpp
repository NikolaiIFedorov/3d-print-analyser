#pragma once

#include <cstdint>

/// One-at-a-time probes for depth fighting / wrong occlusion / hover picking.
/// Change `kActive`, rebuild, run the same camera orientation that misbehaved, note result, then try the next.
///
/// Ortho near/far are tightened from scene + grid + axis bounds in `Display` by default
/// (`kTightenOrthoClipPlanes` in `display.cpp`). These experiments only adjust other GL / pick behavior.
///
/// Suggested order for remaining probes:
/// 1. `BackFaceCull` — `GL_CULL_FACE` on for the 3D pass (stencil/axes path unchanged).
/// 2. `NoWireZBias` — wireframe / pick-highlight lines skip the clip-space Z nudge in `line.vert`.
/// 3. `NoPickPolygonOffset` — pick highlight fill has no `glPolygonOffset`.
/// 4. `PickFrontFaceOnly` — CPU ray pick skips hits on the back side of the triangle (winding vs ray).
///
/// `TightOrthoClip` is kept as an enum value for log ordinals compatibility; it no longer changes clip planes.
enum class ViewportDepthExperiment : std::uint8_t
{
    Baseline = 0,
    TightOrthoClip,
    BackFaceCull,
    NoWireZBias,
    NoPickPolygonOffset,
    PickFrontFaceOnly,
};

namespace ViewportDepthExperiments
{
// Edit only this line between runs:
inline constexpr ViewportDepthExperiment kActive = ViewportDepthExperiment::BackFaceCull;

[[nodiscard]] constexpr ViewportDepthExperiment Active() noexcept { return kActive; }

[[nodiscard]] constexpr bool IsBackFaceCull() noexcept { return kActive == ViewportDepthExperiment::BackFaceCull; }
[[nodiscard]] constexpr bool IsNoWireZBias() noexcept { return kActive == ViewportDepthExperiment::NoWireZBias; }
[[nodiscard]] constexpr bool IsNoPickPolygonOffset() noexcept { return kActive == ViewportDepthExperiment::NoPickPolygonOffset; }
[[nodiscard]] constexpr bool IsPickFrontFaceOnly() noexcept { return kActive == ViewportDepthExperiment::PickFrontFaceOnly; }
} // namespace ViewportDepthExperiments
