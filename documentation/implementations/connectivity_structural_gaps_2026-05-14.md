# Connectivity structural gaps — evaluation hardening (2026-05-14)

## Problem

`EvaluateAppInvalidTagsForSolid` counted directed uses **per `Edge*` only**. That missed:

1. **Split geometric segments** — several straight `Edge*` records for the same physical segment (distinct `Point*` after import), each with ≤2 uses, so edge-wise tags stayed “clean” while the segment was actually multiply covered.
2. **Vertex-only non-manifold** — multiple surface sheets meeting at a point without sharing one `Edge*` (edge-wise scan never fires).

## Plan

- Extend per-half-edge storage to include **`Face*`** so merged cluster classification can dedupe identical `(face, start, end)` tuples.
- **Straight segments:** bucket straight edges by undirected weld-grid key of endpoint positions; union within bucket when endpoint distances match `WeldEpsilonFromSolid`-scale tolerance; aggregate directed uses per DSU component; reuse open / non-manifold / inconsistent orientation rules on the merged multiset (with dedupe).
- **Curved / bridged edges:** keep per-`Edge*` classification (no geometric clustering).
- **Vertices:** build **link graph** at each `Point*` (neighbor pairs from consecutive loop corners); detect duplicate link wedges, degree >2, multiple link components, or components that are neither a simple path nor cycle (`E != V-1` and `E != V`).

## Outcome

**Shipped in `GeometryValidity.cpp` (anonymous-namespace helpers + `EvaluateAppInvalidTagsForSolid`):**

1. **Directed uses carry `Face*`.** Half-edges record `(Face*, start, end)` so merged geometric segments dedupe identical face uses instead of double-counting.

2. **Straight segment clustering.** Straight edges (`curve == nullptr`, `bridgePoints` empty) are unioned when they share the same undirected weld-grid segment key and pass an endpoint-distance coincidence test (`8 * WeldEpsilonFromSolid`). Per-cluster directed uses are classified with the same open / non-manifold / inconsistent-orientation rules as before. Curved/bridged edges stay classified **per `Edge*`** (no geometric merge).

3. **Vertex link graph.** For each `Point*` on the solid, build link edges `(prev, next)` from consecutive loop corners where `GetStart() == p`. Flag `NonManifoldConnectivity` when: duplicate link wedge, link degree > 2, a link component is not a simple path or cycle (`E` not `V-1` or `V`), or more than one link component with edges (e.g. two cones at apex).

**Build:** `cmake --build build --target CAD_OpenGL`.

## Mini retro

- **C++ `goto` over locals:** initial vertex pass used `goto` across `unordered_set` initializations — switched to a per-point immediately-invoked lambda with early `return`.
- **Anonymous vs outer scope:** `DirectedFaceUse` lives in the anonymous namespace; the public evaluator uses `std::tuple<Face*,Point*,Point*>` explicitly so types stay compatible with the helpers in the same TU.

