# Calibrate — hover span preview (second face prerequisite)

## Idea

While in **Calibrate** with face prerequisites, show a world-space span segment plus optional mm label: **committed** picks keep the nominal span; **awaiting point 2** uses hover span when geometry yields a valid preview, otherwise a **cursor-based stub** (centroid → ray/plane) without a label.

## Plan

1. `CalibrateNominal`: expose `SpanPreviewBetweenFaces` returning nominal mm + segment endpoints (parallel faces → along shared normal from first centroid; else centroid–centroid). Refactor `SpanBetweenFaces` to use it (single source of truth).
2. `OpenGLRenderer` / `SceneRenderer`: small dynamic line mesh + draw (line shader, depth-tested).
3. ~~`Display::RebuildPickHighlightMesh`~~ **`Display::RefreshCalibSpanOverlayForViewportRender`** (each `Render`): upload span mesh + label anchors — avoids stale GPU uplinks when only mouse/camera moves.
4. `Display::Render`: after `NewFrame`, project midpoint and draw foreground label.

## Outcome

- Implemented `CalibrateNominal::SpanPreviewBetweenFaces` (endpoints + mm) and routed `SpanBetweenFaces` through it so preview matches nominal slab / centroid fallback.
- **`FaceCentroidWorld`** exported for viewport stubs aligned with span anchors.
- Second Calibrate prerequisite: hover with valid span draws accent segment — **opaque depth pass (5px) + x-ray pass (4px, α 0.45)** like pick-highlight lines so occluded portions still read.
- Preview shows when the second pick would be **rejected** whenever hover yields a valid geometric span; otherwise falls back to the cursor-plane stub (below).
- **Committed picks**: span line + nominal mm label persist while Calibrate is active with both faces picked (until picks cleared).
- **Awaiting second pick, empty hover**: segment from **first-face centroid** to **pick-ray ∩ plane through centroid with normal = view-ray direction** (screen-aligned plane); **no** numeric label.
- Overlay rebuilt **every frame** inside `Render` so orbit/pan/track-pointer stubs stay coherent without relying on `pickDirty`.
- Label placed at the midpoint of the **longest viewport-visible subsegment** (sampled in NDC vs \([-1,1]^3\)), not raw world midpoint (skipped when no measurement label).
- Clean Debug build of `CAD_OpenGL`.

## Mini retro

- Centralizing segment geometry in CalibrateNominal avoided drifting from compensation math.
- Consider later: reuse length-unit formatting from settings if UI moves away from mm-only display.
- Label placement uses viewport clipping only; depth-occluded segments are covered visually by the x-ray line pass so the numeric midpoint aligns with what you see.
- Per-frame overlay refresh trades a tiny CPU/GPU upload cost for correct stub tracking under camera motion (acceptable while Calibrate span is single segment).
