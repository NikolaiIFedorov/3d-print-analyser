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
