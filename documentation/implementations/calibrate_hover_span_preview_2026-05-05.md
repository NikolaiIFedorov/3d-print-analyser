# Calibrate — hover span preview (second face prerequisite)

## Idea

While waiting for the **second** Calibrate face pick (first face already committed), show a world-space segment between the nominal span endpoints used by `SpanBetweenFaces`, plus a short `mm` label. The hover hit defines the candidate face (including rejected second picks where span geometry still exists). Avoids ambiguous “cursor depth” by tying the preview to face hover.

## Plan

1. `CalibrateNominal`: expose `SpanPreviewBetweenFaces` returning nominal mm + segment endpoints (parallel faces → along shared normal from first centroid; else centroid–centroid). Refactor `SpanBetweenFaces` to use it (single source of truth).
2. `OpenGLRenderer` / `SceneRenderer`: small dynamic line mesh + draw (line shader, depth-tested).
3. `Display::RebuildPickHighlightMesh`: when second prerequisite active + valid hover, upload segment + stash label string + midpoint for ImGui.
4. `Display::Render`: after `NewFrame`, project midpoint and draw foreground label.

## Outcome

- Implemented `CalibrateNominal::SpanPreviewBetweenFaces` (endpoints + mm) and routed `SpanBetweenFaces` through it so preview matches nominal slab / centroid fallback.
- Second Calibrate prerequisite: hover with valid span draws accent segment — **opaque depth pass (5px) + x-ray pass (4px, α 0.45)** like pick-highlight lines so occluded portions still read.
- Preview shows even when the second pick would be **rejected** (still requires a hover face and valid span).
- Label placed at the midpoint of the **longest viewport-visible subsegment** (sampled in NDC vs \([-1,1]^3\)), not raw world midpoint.
- Clean Debug build of `CAD_OpenGL`.

## Mini retro

- Centralizing segment geometry in CalibrateNominal avoided drifting from compensation math.
- Consider later: reuse length-unit formatting from settings if UI moves away from mm-only display.
- Label placement uses viewport clipping only; depth-occluded segments are covered visually by the x-ray line pass so the numeric midpoint aligns with what you see.
