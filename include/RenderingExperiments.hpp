#pragma once

/// One-at-a-time rendering probes (rebuild between toggles).
namespace RenderingExperiments
{
// --- Theory #1: wireframe + pick-highlight lines use the GS line pipeline, whose quads can
//     win the same depth samples as nearby filled triangles. Depth-test on read, but do not
//     write depth, so lines cannot stomp patch/highlight depth for later pixels.
// Set `false` to restore legacy behavior (lines write depth).
inline constexpr bool kLineDrawsOmitDepthWrite = true;
} // namespace RenderingExperiments
