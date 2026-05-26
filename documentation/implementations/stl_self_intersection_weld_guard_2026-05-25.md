# STL import self-intersection face-drop guard

## Goal

Prevent binary STL import from collapsing intentional thin/intersecting triangles into degenerate faces, which then shows a false open-boundary invalidation.

## Hypothesis

Binary STL point weld tolerance was too permissive (`~2 ULP` scale with axis-aligned threshold), so near-but-distinct vertices could weld to one `Point`, dropping one triangle before validity tagging.

## Plan

1. Make binary STL weld tolerance conservative (`< 0.5 ULP`).
2. Use Euclidean distance check instead of axis-by-axis cube threshold for nearby-point weld.
3. Rebuild and verify compile.

## Implemented

- Updated `BinaryStlWeldToleranceFromBounds` in `src/logic/Import/STLImport.cpp`:
  - tolerance now `max(1e-15, 0.49 * ulpAtScale)`.
  - comment clarified intent: never collapse adjacent representable float coordinates.
- Updated `GetOrCreateNearbyFloatPoint` in `src/logic/Import/STLImport.cpp`:
  - replaced per-axis absolute checks with squared-distance check against `tolerance^2`.

## Validation

- `cmake --build . --target CAD_OpenGL` succeeded from `build/`.

## Outcome

Importer now preserves skinny/intersecting STL triangles more faithfully while still deduplicating true near-identical vertices introduced by float noise.

## Follow-up (same day, user retest)

User still reproduced missing-face behavior on a point-edge overlap case (two extruded triangles). Runtime import counts suggested a pre-solid collapse (e.g., `2.stl` importing with fewer faces than expected), so binary STL dedup was tightened further:

- binary vertex dedup now matches exact float bit patterns (no tolerance-based weld),
- this preserves distinct exported vertices even when they are extremely close in world space,
- exact duplicates still dedup naturally.

Rebuilt successfully again (`cmake --build . --target CAD_OpenGL`).

## Follow-up 2 (point-edge overlap case)

User reported no behavioral change and noted that pressing open-boundary `Fix` also did not change geometry. Root cause was likely post-import topology repair (inside `Scene::CreateSolid`) mutating STL solids before fix handlers run:

- `TryRepairDegenerateSolidBRep` and duplicate-edge merge were still running for STL solids unless a specific self-intersection tag was already present,
- point-edge overlap can be problematic without tripping that exact self-intersection detector.

Applied change:

- `Scene::CreateSolid` now accepts `runTopologyRepairs` (default `true`),
- STL import now calls `CreateSolid(faces, false)` for both binary and ASCII paths,
- with repairs disabled, imported STL topology is preserved as-is and only validity cache is refreshed.

Rebuilt successfully after this change as well.

## Follow-up 3 (temporary cleanup-off debug)

Per user request, disabled STL coplanar merge cleanup entirely for isolation:

- `GeometryExperiments::kSkipStlMergeCoplanarFaces = true`.

This keeps imported STL face topology untouched by merge pass so we can verify whether cleanup is still causing the missing intersecting face. Rebuilt successfully.

## Follow-up 4 (guarded merge restore)

After confirming cleanup-off avoided the false open-boundary regression, re-enabled coplanar merge with stricter guards:

- `GeometryExperiments::kSkipStlMergeCoplanarFaces` returned to `false`,
- merge now skips when cached validity already reports risky topology:
  - `SelfIntersection`, `NonManifoldConnectivity`, or `OpenBoundary`,
- merge also skips when baseline topology histogram reports `edgesThreePlusBefore > 0` (non-manifold edge sharing).

Intent: keep merge for ordinary clean STL meshes while preserving raw topology for overlap/non-manifold cases that previously produced face drops/open-boundary false positives.

## Follow-up 5 (guard tuning)

Observed import diagnostics still skipping merge on overlap sample due `edges_3plus_faces_before = 1`, leaving triangles unmerged.

Tuning applied:

- removed the histogram-based skip (`edgesThreePlusBefore > 0`) from STL merge gate,
- kept validity-tag gate (`SelfIntersection`, `NonManifoldConnectivity`, `OpenBoundary`) as the safety check.

This restores merge attempts for overlap cases that are not explicitly tagged invalid at baseline, while still skipping known risky tagged solids.

## Follow-up 6 (safeguards removed for A/B)

Per user request and observed "no merge" behavior, removed STL merge safeguard gating entirely from `MergeStlCoplanarMaybe` and restored pre-guard behavior:

- no pre-merge topology tag checks in STL import merge path,
- merge runs whenever `kSkipStlMergeCoplanarFaces == false`.

Rebuilt successfully after removal.

## Follow-up 7 (confirmed merge root cause + targeted fix)

User re-test after removing safeguards confirmed regression returns: intersecting face disappears when merge runs.

Root cause in merge path:

- `Scene::MergeCoplanarFaces` disconnected every shared edge between merge candidates,
- but in overlap/self-intersection zones some "shared" edges can be non-manifold (`dependencies` includes 3+ faces),
- disconnecting such edges corrupts neighboring faces (observed as dropped face + open boundary).

Targeted fix:

- before merging a face pair, require every shared edge to be owned exclusively by that pair:
  - `dependencies.size() == 2`,
  - and dependencies contain both candidate faces.
- otherwise skip that merge pair and continue scanning.

Result: regular manifold coplanar merges still run, while non-manifold overlap intersections no longer lose faces due to edge teardown.

## Follow-up 8 (split-fix path topology preservation)

User observed a similar "faces absent" symptom when self-intersection fix could not split cleanly. Potentially same family of issue: split-created solids were still built with default topology repairs.

Adjustment:

- in `SplitSolidIntoFaceConnectedComponents` (`display.cpp`), new component solids now use:
  - `scene->CreateSolid(componentFaces, false)`.

Rationale:

- split pieces from self-intersection/non-manifold regions should preserve raw topology,
- avoids generic repair passes removing faces and making split attempts appear destructive.

## Follow-up 9 (non-destructive split failure)

Another similar failure mode identified in UI fix dispatch for combined tag (`IMPORT_GEOM_OPENBOUNDARY_SELF_INTERSECTION`):

- previous behavior: if self-intersection split failed, same click still ran open-boundary fix (`splitFixed || TryFixOpenBoundary...`),
- this could mutate topology even though split itself failed, making failure appear destructive.

Change:

- for combined tag, run open-boundary fix only after a successful split,
- when split fails, do not run fallback repair in the same action; return failure as non-mutating.

Build succeeded after change.

## Follow-up 10 (open-boundary missing-face visualization)

User requested more specific blame rendering: highlight the inferred missing face area, not only its boundary edges.

Implemented:

- added a dedicated open-boundary **face** overlay GPU path:
  - `UploadOpenBoundaryBlameFaceMesh` / `DrawOpenBoundaryBlameFace` in OpenGL renderer,
  - scene-renderer wrappers to upload/render this pass.
- in `Display`, added face-overlay mesh buffers and rebuild step:
  - infer candidate cap loops with the same boundary-loop logic used by open-boundary repair,
  - group loops into face-loop sets,
  - triangulate projected loops using earcut,
  - upload translucent filled polygons in accent color.
- rendering now draws:
  - translucent filled inferred missing-face overlay (front + xray),
  - existing thick boundary/context lines on top.

Result: blame is now spatially specific to the inferred missing face region rather than only edge traces.

## Follow-up 11 (pre-merge self-intersection gate)

User hypothesis: merge may be turning self-intersection topology into apparent open-boundary regions.

Implemented in STL import:

- before any coplanar merge step, evaluate raw validity tags on the just-created solid,
- if `SelfIntersection` is present, skip merge entirely for that import,
- keep raw tags cached and collect baseline merge diagnostics without mutating topology.

Intent: prevent merge from introducing/expanding false open-boundary blame in self-intersecting imports while preserving existing merge behavior for non-self-intersecting meshes.
