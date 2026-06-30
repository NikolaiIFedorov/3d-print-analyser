# Rendering — Future TODOs

## Adaptive curve tessellation — DONE 2026-06-20
`Wireframe::TessellateCurve` previously fixed every curve at 16 segments regardless of size/curvature. Replaced with recursive world-space chord-deviation tessellation (`TessellateCurveRecursive`): starts from 4 base spans, recursively splits each based on midpoint deviation from the chord vs. a 0.1mm tolerance (matching `GCPnts_QuasiUniformDeflection`'s tolerance on the OCCT edge path), capped at depth 7 (max 512 segments/curve). Camera-independent — runs only at chunk rebuild, never per-frame.

## GPU tessellation shaders (longer-term, deliberate project)
Considered when first writing the renderer; deferred due to (1) GL4.0+ hardware requirement and (2) pipeline complexity (TCS/TES stages, `GL_PATCHES` primitives, per-patch tessellation levels).

Revisit if a ~2010+/GL4 hardware floor is acceptable for target users (already roughly true today). Gives true continuous LOD with no CPU-side rebuild on zoom/orbit at all. Bigger lift than the tolerance fix above — treat as its own scoped project, not a quick follow-on. Note: still wouldn't replace tolerance-based tessellation for OCCT/face-backed edges, only direct curve-edge rendering.

## Pan/orbit/zoom lag from per-frame hover picking — DONE 2026-06-20
Diagnosed via timing instrumentation (`LogSlowStage`-style logging added to `UpdatePickHover`, `PickClosestFace`, `PickClosestEdgeAlongRay`, `Render()` stages): panning in the Structure tool was laggy because `Display::Frame()` called `UpdatePickHover` on every frame the camera moved (`cameraMovedForPick` gate), re-running a brute-force O(N) scan over all pick triangles (`ScenePick.cpp`'s `PickClosestFace`, no spatial acceleration) plus a second full scan in `RebuildPickHighlightMesh` whenever the hovered face changed — every single frame during a pan. Confirmed via profiling: ~2286 triangles, ~1.5-2ms/scan, running on every camera-moved frame; logs showed zero such entries once disabled, and panning became smooth.

Root cause: trackpad two-finger pan/orbit and scroll-wheel zoom (`Input.cpp:177-283`) call `Pan`/`Orbit`/`Zoom` directly, bypassing the mouse-motion handler's existing nav-drag guard (`Input.cpp:350-360`, which already correctly skips hover during RMB/MMB drag). `Frame()`'s blanket `UpdatePickHover` call was the only thing re-triggering the scan for those gesture/wheel-driven camera moves.

Fix: removed the per-frame `UpdatePickHover` call from `Frame()` entirely (`display.cpp`, near the old `cameraMovedForPick` block). Hover now updates only from input events — mouse motion when not navigating, and explicit calls on RMB/MMB release — which was already sufficient and correctly excluded drag-nav. Accepted tradeoff: hover may go briefly stale mid pure-gesture/wheel-zoom (no live update while literally mid-gesture with no mouse motion), refreshing on the next real motion event or nav release — acceptable since face selection isn't expected while navigating.

Not fixed (deferred): the pick scan itself is still O(N) brute-force with no spatial acceleration (BVH/grid). Revisit if scenes grow large enough that even motion-driven hover picking becomes slow, or if live hover during navigation is wanted later.

## Vulkan migration (separate, large, not currently justified by multithreading)
Considered as a way to get multithreading, but that reasoning doesn't hold: Vulkan's multithreading benefit is multithreaded *command buffer recording*, which only pays off with thousands of draw calls/frame. Current renderer batches to ~12 draw calls/frame (see `OpenGLRenderer.cpp`), so that bottleneck doesn't exist here.

The multithreading actually worth having — parallelizing curve tessellation, solid rebuilds, GeometryOps boolean ops — doesn't require Vulkan; it can be done today via the existing `TaskRunner` thread pool (`src/utils/TaskRunner.hpp`) under OpenGL.

If Vulkan migration happens, it should be justified on its own terms (e.g. wanting the lower-level API, future engine direction) — not as a means to multithreading. Scope: replace `OpenGLRenderer.cpp` (~1700 lines) + shader management; CPU-side chunking/caching architecture (`SceneRenderer`, `Wireframe`, `Patch`) is largely API-agnostic and reusable.
