## 2026-05-01 — Ortho depth clip vs curved geometry; RMB pan without relative-mouse grab

### Problem
- Zoomed navigation sometimes clipped meshes: tightened ortho near/far used `scene->points` plus grid/axes, but shaded/wire meshes tessellate **curves** whose interior extends beyond endpoints (and bridge points matter for depth).
- RMB pan felt slow to start: SDL relative mouse mode was enabled for **both** MMB orbit and RMB pan; enabling grab can stall or zero the first relative deltas.

### Plan
1. In `ApplyOrthoClipFromViewBounds`, add samples along every curved edge (same Evaluate-based sampling idea as Patch) plus explicit `bridgePoints` positions.
2. Enable `SDL_SetWindowRelativeMouseMode` only while **middle** button orbit is active; leave cursor “normal” for RMB pan (motion still uses `xrel`/`yrel`).

### Outcome
- **Clip:** `ApplyOrthoClipFromViewBounds` now folds in `bridgePoints` and multi-sample curved edges via `curve->Evaluate`, matching shaded patch extent beyond pure vertex hulls for the same tightened slab.
- **Pan:** Relative mouse applies only during MMB orbit; RMB pan uses normal tracking so motion deltas start immediately.
- Build: `cmake --build build -j4` succeeds.

### Mini retrospective
- **Worked:** Narrow, local changes (same helper path as Patch’s curve sampling philosophy; input one-line semantic change).
- **Follow-up:** Highly tessellated NURBS *surfaces* could still underestimate if interior bulges exceed control hull (unlikely vs edges); revisit with patch mesh AABB if observed.

## 2026-05-01 (follow-up) — Grid/axes clipping near camera

### Problem
- After the previous fix, model clipping stopped, but grid and axis lines still clipped when very close.
- Cause: shader Z-layer bias (`uClipZBiasW`) is applied in clip-space; near clip boundaries that offset can push only biased line primitives outside `[-w, +w]`.

### Fix
- In `basic.vert` and `line.vert`, compute biased clip Z then clamp back to `[-w + eps, +w - eps]` with a tiny margin.
- This preserves depth layering while preventing bias-induced clip rejection at near/far planes.

### Validation
- Build passes: `cmake --build build -j4`.

### Follow-up 2
- Remaining issue: near-origin zoom still clipped some axis segments while grid stayed visible.
- Root cause: `RenderAxes()` required `stencil == 0`; stencil coverage near geometry/origin could mask axis lines even when depth would allow them.
- Fix: remove stencil gate for axes and rely on depth test only.

### 2026-05-04 Follow-up — Camera-orientation face clipping

Problem: filled faces appeared to be clipped/disappearing depending on the camera orientation after
the depth investigation probes. The active viewport depth probe was `BackFaceCull`, which enables
`GL_CULL_FACE` for filled patches and pick highlights.

Cause: imported STL geometry and the current coplanar-merge output do not guarantee globally
consistent outward winding for every face loop. Back-face culling therefore hides some otherwise
valid CAD faces when viewed from the side OpenGL treats as back-facing.

Fix: restore `ViewportDepthExperiments::kActive` to `Baseline` and leave back-face culling as an
explicit one-run probe only. The viewport should render CAD faces two-sided until the scene/import
pipeline can prove consistent shell orientation.

Validation: `cmake --build build -j4` passes.

Mini retro: this was a probe-state leak rather than a new clipping-plane bug. Keeping the experiment
available but making the default two-sided preserves the investigation tool without exposing users to
orientation-dependent missing faces.

### 2026-05-04 Follow-up — Axis mesh length sync

Problem: axes showed a similar orientation-dependent clipping/shortening artifact even after the
face culling probe was reset.

Cause: `Display::SyncViewportAxisForDepthClip()` computed a new view-dependent axis half-length,
but `ViewportRenderer::SetAxisWorldHalfExtent()` only stored the value. The axis vertices are baked
into the same static line mesh as the grid, so the GPU kept drawing the previous axis length until an
unrelated grid LOD rebuild happened.

Fix: make `SetAxisWorldHalfExtent()` regenerate the viewport line mesh when the half-length changes
materially. The existing `Display` deadband still limits rebuild frequency.

Validation: `cmake --build build -j4` passes.

Mini retro: this was a CPU/GPU state sync bug hidden behind the grid's unrelated rebuild path. Keeping
the rebuild inside the setter makes the viewport renderer own its baked mesh invariant.
