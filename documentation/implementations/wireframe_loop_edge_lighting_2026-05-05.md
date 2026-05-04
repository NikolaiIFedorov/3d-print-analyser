# Wireframe loop edges — match patch lighting (2026-05-05)

## Idea

Loop edges (face boundary edges drawn with `isFace == true`) should use the same directional diffuse model as shaded faces so edges stay visible on bright lit regions.

## Approach

- **CPU:** For each such edge, among `edge->dependencies`, pick the face whose unit normal maximizes `max(0, dot(N, L))` with the same `L` as `SceneLighting::DirectionalLightDirWorld()` (shared with `OpenGLRenderer` / `basic.frag`).
- **Vertex:** Store that face normal on the line vertices (`Vertex::normal`); loose / non-loop edges keep `normal = 0`.
- **GPU:** Extend `line.vert` / `line.geom` / `line.frag` to apply the same diffuse term as `basic.frag` when `uLightingEnabled` is on and the transformed normal is non-degenerate; `DrawPickHighlightLines` forces lighting off.

## Outcome

- Loop / solid-boundary edges (`AddEdge` with `isFace`): `Vertex::normal` is the unit normal of the adjacent **face with largest** `max(0, dot(N, L))` under `SceneLighting::DirectionalLightDirWorld()`, matching “use the brighter-lit face’s normal.”
- Loose edges and sketches: `normal` stays zero; `line.frag` skips lighting (same flat color as before).
- `SceneLighting` centralizes `DirectionalLightDirWorld` and `SceneMeshBrightenAmount` so CPU edge picking matches `basic.frag` / `DrawTrianglesPass`.
- Pick-highlight lines force `uLightingEnabled = 0` so selection outlines stay unshaded.

Files: `include/rendering/SceneLighting.hpp`, `Wireframe.{hpp,cpp}`, `OpenGL/shaders/line.{vert,geom,frag}`, `OpenGLRenderer.cpp`.

## Mini retro

Centralizing light direction / brighten amount avoided duplicating magic numbers between CPU edge picking and GPU. Geometry shader I/O change was straightforward; pick-highlight path needed an explicit lighting-off uniform.
