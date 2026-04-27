#pragma once

/// One-at-a-time rendering probes (rebuild between toggles).
namespace RenderingExperiments
{
// --- Debug sanity: skip `SceneRenderer::RenderWireframe` (B-Rep / mesh edge lines only).
//     If only edges vanish, blame the line pass; if lighting / analysis tint / hover fills
//     still layer wrong, the issue is **triangle depth** (or mesh), not this draw alone.
//     Pick-highlight lines and grid/axes are unchanged.
inline constexpr bool kDebugSkipSceneWireframe = false;

// --- Default framebuffer MSAA: 0 = off (explicit `SDL_GL_MULTISAMPLEBUFFERS` 0). Use 2, 4, or 8
//     to test with multisampling — restart required. `OpenGLRenderer` enables `GL_MULTISAMPLE`
//     only when this is > 0 and logs `GL_SAMPLES` / `GL_SAMPLE_BUFFERS` after context creation.
inline constexpr int kGlFramebufferMsaaSamples = 0;

// --- Depth precision (phase 1): reverse-Z — `ProjectionDepthMode::EffectiveProjection`,
//     `glClearDepth(0)`, `GL_GEQUAL`; wire Z nudge sign flips. Improves ULP spacing along the ray
//     for perspective; for **ortho** + **coplanar / equal-Z** fights, visuals may barely change
//     (reverse-Z does not break ties). Log line confirms mode at startup in `OpenGLRenderer`.
inline constexpr bool kReverseZDepth = false;

// --- Mesh / numeric (phase 2): only when `kCullDegeneratePatchTriangles` is true — skip earcut
//     tris with `length(cross(e1,e2)) < kDegeneratePatchMinCrossLen`. The **bool** turns culling
//     on/off; the **double** is the threshold only (not a boolean). Reload scene after changes.
//     Trial: off vs on and 1e-13–1e-15 showed **no** visible change → likely not zero-area tris.
inline constexpr bool kCullDegeneratePatchTriangles = false;
inline constexpr double kDegeneratePatchMinCrossLen = 1e-15;

// --- Wire depth write: GS line quads can win the same depth samples as nearby filled tris.
//     When true, lines depth-test on read but omit depth writes so they do not stomp patch depth.
inline constexpr bool kLineDrawsOmitDepthWrite = false;

// --- Theory #2 (line vs patch depth): `line.vert` clip-space Z nudge. Shader uses 1e-6 × this;
//     legacy implicit scale was 1. Larger values pull wire quads toward camera vs coplanar patches.
//     `ViewportDepthExperiments::NoWireZBias` forces nudge to 0 on CPU.
//     Bumped from 1 → 48 after reports of edge/face z-fighting in ortho (still subtle; tune if lines pop).
inline constexpr float kWireframeClipZNudgeScale = 48.0f;

// --- Theory #2b: `GL_POLYGON_OFFSET_FILL` on wireframe / pick-highlight *line* draws (GS emits
//     filled tris). Default off — trial did not materially change ghosting; set true to re-test.
inline constexpr bool kWireframeLinePolygonOffsetDeeper = false;

// --- Theory #2c: pick highlight fill draws without glPolygonOffset (no pull toward camera).
// Independent of `ViewportDepthExperiments`; either this or `NoPickPolygonOffset` skips offset.
inline constexpr bool kPickHighlightNoPolygonOffset = false;

// --- Theory #3: back-face cull only for filled patches + pick highlight (not grid/lines/axes).
// `ViewportDepthExperiments::BackFaceCull` uses the same window.
inline constexpr bool kCullBackFacesOpaquePatches = false;

// --- Theory #4: depth prepass for opaque patches (color mask off, then full color + depth).
inline constexpr bool kDepthPrepassOpaquePatches = false;
} // namespace RenderingExperiments
