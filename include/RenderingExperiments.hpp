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

// --- macOS built-in trackpad: `SDL_HINT_TRACKPAD_IS_TOUCH_ONLY` (restart required).
//     `true`: two-finger drags use `FINGER_*` (touch-centroid pan) and often duplicate `MOUSE_WHEEL`.
//     `false`: trackpad behaves like mouse + wheel only — simpler, no touch pan path.
inline constexpr bool kSdlTrackpadIsTouchOnly = false;

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

// --- Ortho reference depth: `basic.vert` / `line.vert` use `gl_Position.z += uClipZBiasW * w`.
//     Keep grid on the scene mesh layer so the floor grid does not pull through faces in snapped
//     principal views. Axes use `ViewportRenderer::AxesClipZBiasW()` — base delta `kClipZBiasGridAxesDeltaW`
//     scaled by ortho depth span and zoom so separation stays above depth ULPs when zoomed out; grid
//     lines vs axes after the grid depth write. `ClipZBias*W()` flips sign for reverse-Z.
inline constexpr float kClipZBiasSceneMeshW = 0.0022f;
inline constexpr float kClipZBiasGridW = kClipZBiasSceneMeshW;
inline constexpr float kClipZBiasGridAxesDeltaW = 4.0e-5f;
inline constexpr float kClipZBiasAxesW = kClipZBiasGridW - kClipZBiasGridAxesDeltaW;

inline float ClipZBiasSceneMeshW()
{
    return kReverseZDepth ? -kClipZBiasSceneMeshW : kClipZBiasSceneMeshW;
}
inline float ClipZBiasGridW()
{
    return kReverseZDepth ? -kClipZBiasGridW : kClipZBiasGridW;
}
inline float ClipZBiasAxesW()
{
    return kReverseZDepth ? -kClipZBiasAxesW : kClipZBiasAxesW;
}

// --- Theory #2b: `GL_POLYGON_OFFSET_FILL` on wireframe / pick-highlight *line* draws (GS emits
//     filled tris). Default off — trial did not materially change ghosting; set true to re-test.
inline constexpr bool kWireframeLinePolygonOffsetDeeper = false;

// Pick highlight fill stays honestly depth-tested; occluded selected/hovered portions are handled by
// the translucent x-ray pass instead of pulling the opaque pass toward the camera.
inline constexpr bool kPickHighlightNoPolygonOffset = true;

/// Alpha for occluded-face x-ray pass (`DepthCompareBehind` + blend); keep modest — dense tessellation can read noisy.
inline constexpr float kPickHighlightFaceXrayAlpha = 0.14f;

/// Calibrate: second-pick **blocked** hover — same lit pick mesh as valid hover but **neutral UI gray**
/// (`Color::GetUI` depth). Dark mode: smaller depth = darker; light mode: larger depth = darker.
inline constexpr int kCalibrateRejectHoverGrayUiDepthDark = 1;
inline constexpr int kCalibrateRejectHoverGrayUiDepthLight = 5;

/// When true, dim every non-parallel face while waiting for Calibrate point 2. Default off — blocked state is
/// hover-only (darker neutral gray on the face under the cursor).
inline constexpr bool kCalibrateSecondPickDrawInvalidFacePool = false;

/// Used only when `kCalibrateSecondPickDrawInvalidFacePool` is true (full-model invalid veil).
inline constexpr float kPickHighlightCalibInvalidPoolAlpha = 0.28f;

// --- Theory #3: back-face cull only for filled patches + pick highlight (not grid/lines/axes).
// `ViewportDepthExperiments::BackFaceCull` uses the same window.
inline constexpr bool kCullBackFacesOpaquePatches = false;

// --- Theory #4: depth prepass for opaque patches (color mask off, then full color + depth).
inline constexpr bool kDepthPrepassOpaquePatches = false;
} // namespace RenderingExperiments
