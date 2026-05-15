# `NonManifoldConnectivity` — next invalid case (plan only, 2026-05-14)

Follow-on after **OpenBoundary** UX design (`import_open_boundary_ux_2026-05-14.md`). No code changes in this note.

## What the tag means (app layer)

From `EvaluateAppInvalidTagsForSolid`: an `Edge` has **more than two** directed uses from the solid’s face loops, **or** exactly two uses whose endpoint pairs are **neither** opposing **nor** identical (same-directed — the latter is classified as `InconsistentFaceOrientation` instead).

Typical sources: **internal partitions**, **T-junctions** gone wrong, **zero-thickness** sheets, bad merges, or hostile STL topology.

## Why not auto-repair in `CreateSolid` (yet)

Repairs are **under-specified**: splitting non-manifold vertices, deleting internal faces, or CGAL-style soup repair can **change intent**. Same as open-boundary “patch”: prefer **detect → explain → explicit command with preview** before silent fixes.

## Suggested phases

1. **Surface in UI** — Same diagnostics surface as open boundary; message like “Non-manifold edge — this operation may fail.”
2. **Tool gating** — Refuse or warn when CGAL / Structure requires manifold-like soup; log `DescribeAppInvalidTagsForLog` on carve failure (partially already done in `StructureCarve`).
3. **Targeted repair** — One operation at a time (e.g. `duplicate_non_manifold_vertices` is already used on CGAL mesh paths; B-rep-level repair is separate work).
4. **Tests** — Small synthetic solids that set the tag; ensure repair passes do not regress orientation repair.

## Relation to other tags

- Often co-occurs with **CGAL soup** failures; keep **`CgalPolygonSoupTag`** and **`AppInvalidTag`** distinct in UI copy.
- **OpenBoundary** = one side missing; **NonManifold** = too many or inconsistent pairings — different user explanations.

## Outcome

Next engineering focus after OpenBoundary **UX Phase A** can be **non-manifold messaging + gating**; repair remains **explicit** until a chosen algorithm is owned and tested.
