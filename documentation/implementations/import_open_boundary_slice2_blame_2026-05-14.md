# Import open boundary — slice 2 (3D blame lines)

## Goal

After slice 1 (contract + banner), show **where** the open boundary is: highlight contributing **`Edge`** segments in the viewport using the existing pick-highlight **line** pass.

## Approach

- **`GeometryValidity::CollectOpenBoundaryEdgesForSolid`** — builds the same directed `edgeUses` map as connectivity evaluation (face-loop walk), calls `EvaluateEdgeConnectivityTags` with an optional **`openBoundaryEdgesOut`** list instead of duplicating DSU / weld logic.
- **`EvaluateEdgeConnectivityTags`** — fourth parameter `std::vector<const Edge *> *openBoundaryEdgesOut`; when a directed-use group is exactly one unique incidence (same rule as `OpenBoundary` for straight groups and curved edges), push a representative `Edge *`.
- **`SortUniqueDirectedUsesInPlace`** — factored from `AppendTagsFromDirectedUses` so tag logic and blame collection share one dedup ordering.
- **`Display::RebuildImportOpenBoundaryBlameEdges`** — when `calibStepImportClosedVolume` is **Active**, merges per-solid open-boundary edges into `importOpenBoundaryBlameEdges` (deduped); clears when contract **Done**; **`MarkPickDirty()`** so `RebuildPickHighlightMesh` runs.
- **`RebuildPickHighlightMesh`** — draws blame edges with accent colour via existing `appendEdgeLinesRgb` + pick segment soup.

## Outcome

Shipped; `cmake --build build --target CAD_OpenGL` succeeds.

## Bugfix (Calibrate crash)

`uiCalibrate->children.reserve(size + 1)` then two root `AddParagraph` calls **after** caching `calibPara_*` into nested `Prerequisites` reallocated the root vector and **invalidated** those pointers (same pattern as Structure HoverHint). **Fix:** reserve adequate headroom and append **Processing** + **OpenBoundaryBanner** paragraphs **before** `FindSection` / pointer capture.

## Bugfix (blame lines invisible)

Blame drew only when `PickSegment::edge` matched the collected `Edge*` (missed welded-straight **sibling** records in the same open group). **Fix:** collect **all** edges in an open weld group; draw blame from **edge geometry** (endpoints / curve / bridge) instead of pick-segment lookup; brighter accent line colour.

## Follow-ups

- Rebuild blame after **topology-changing** edits without import/tab (same hook family as slice 1 refresh).
- Optional: soften colour / depth bias if blame fights Structure eligible tint.

## Mini retro

Pushing optional edge list through `EvaluateEdgeConnectivityTags` avoided a large copy of the weld/DSU block; if more tags need geometry subsets, consider a small visitor or structured connectivity result type.

## 2026-05-25 — intuitive missing-face context trial

User validation showed strict boundary-only blame can feel incomplete: one edge visually expected as part of the missing cap was not highlighted. We kept strict boundary edges as canonical truth and added an inferred context set in `Display::RebuildImportOpenBoundaryBlameEdges`:

- Build `boundaryVertices` from strict open-boundary edges.
- Scan solid loops for non-boundary edges connected to those vertices.
- Prefer edges where **both endpoints** are boundary vertices; if none exist, fall back to one-endpoint candidates.
- Upload/draw context in the dedicated open-boundary line pass (`importOpenBoundaryContextEdges`) with a softer accent than strict edges.

Result: view now reads closer to “all edges around the missing face” without changing the underlying gating/truth rule (`OpenBoundary`).
