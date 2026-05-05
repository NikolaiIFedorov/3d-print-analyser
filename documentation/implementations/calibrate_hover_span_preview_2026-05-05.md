# Calibrate — hover span preview (second face prerequisite)

## Idea

While waiting for the **second** Calibrate face pick (first face already committed), show a world-space segment between the nominal span endpoints used by `SpanBetweenFaces`, plus a short `mm` label at the midpoint. Only when hovering a **valid** face (same rules as commit — not rejected parallel/workflow). Avoids ambiguous “cursor depth” by tying the preview to the hover hit’s face.

## Plan

1. `CalibrateNominal`: expose `SpanPreviewBetweenFaces` returning nominal mm + segment endpoints (parallel faces → along shared normal from first centroid; else centroid–centroid). Refactor `SpanBetweenFaces` to use it (single source of truth).
2. `OpenGLRenderer` / `SceneRenderer`: small dynamic line mesh + draw (line shader, depth-tested).
3. `Display::RebuildPickHighlightMesh`: when second prerequisite active + valid hover, upload segment + stash label string + midpoint for ImGui.
4. `Display::Render`: after `NewFrame`, project midpoint and draw foreground label.

## Outcome

- Implemented `CalibrateNominal::SpanPreviewBetweenFaces` (endpoints + mm) and routed `SpanBetweenFaces` through it so preview matches nominal slab / centroid fallback.
- Second Calibrate prerequisite: valid hover (non-rejected) draws accent segment (`RenderCalibHoverSpanLine`, 5px) and ImGui foreground label `"%.3f mm"` at midpoint (shadow for contrast).
- Clean Debug build of `CAD_OpenGL`.

## Mini retro

- Centralizing segment geometry in CalibrateNominal avoided drifting from compensation math.
- Consider later: reuse length-unit formatting from settings if UI moves away from mm-only display.
