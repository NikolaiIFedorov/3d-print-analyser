# Structure translucent shell — back faces opaque (2026-05-11)

## Idea

While the Structure tool shows imported solid shells with translucency, use `gl_FrontFacing` so **camera-facing** triangles keep the configured alpha and **interior-facing** (back) triangles render **opaque** (`alpha = 1`), reducing stacked-glass washout and clarifying the hull.

## Plan

1. Extend `basic.frag` with `uStructureShellBackFaceOpaque` (0 = legacy behavior, 1 = back-face alpha override).
2. Enable the flag only on the **second** draw of the structure split path (translucent color pass after depth prepass); keep 0 on all other `basic` draws (pick highlight, viewport grid/axes, depth prepass, loose geometry).

## Outcome

Implemented as above. `OpenGLRenderer::DrawTriangles` structure split and `ViewportRenderer` set the uniform explicitly so line/grid paths never rely on undefined `gl_FrontFacing` with the flag enabled.

**Build:** `cmake --build build --target CAD_OpenGL` succeeded (2026-05-11).

## Mini retro

- Central flag on one uniform kept the change localized to `basic.frag` and draw setup.
- Any future draw that uses `basic` without setting the new uniform risks stale state; we set `0` on every known path; if a new path appears, add the uniform there.

## Follow-ups

- If winding on some imports is inverted, front/back roles swap; treat as mesh QA or add a user toggle if needed.
