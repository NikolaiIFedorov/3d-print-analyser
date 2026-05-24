# Import open boundary — slice 3 (Fix action, v1)

## Goal

Wire the Calibrate `Fix` action so users can attempt an in-app repair for open-boundary imports instead of only exiting and reworking externally.

## First-pass scope (implemented)

- Add `Display::TryFixOpenBoundaryForActiveScene()`.
- Enable `Fix##oobFix` button in `CalibOpenBoundaryBanner`.
- Conservative repair strategy:
  - collect strict open-boundary edges per solid;
  - accept only linear edges (`curve == nullptr` and no `bridgePoints`);
  - infer closed loops only when every boundary vertex has degree 2;
  - create planar cap face(s) from inferred loop edge order;
  - attach created faces to the owning `Solid`.
- After repair:
  - run per-solid `MergeCoplanarFaces`;
  - refresh `GeometryValidity` caches;
  - refresh import closed-volume contract + blame overlay + gating/UI dirty flags.

## Failure behavior

If loop inference cannot produce a closed linear loop, keep model loaded and show:

- code: `IMPORT_OPEN_BOUNDARY_FIX_FAILED`
- message: automatic fix could not infer a closed linear boundary loop.

## Notes

- This is intentionally narrow and safe for the current test case.
- Curved/open boundaries and complex non-manifold cases are deferred to later iterations.

## 2026-05-25 update — component-wise repair

Initial v1 required the entire boundary graph to be one perfect degree-2 loop; mixed topology produced no repair even when some loops were fixable.

Updated behavior:

- detect connected components of linear boundary edges;
- repair every component that is a valid closed degree-2 cycle;
- skip unresolved components and continue with valid ones;
- when unresolved boundaries remain after repair, show `IMPORT_OPEN_BOUNDARY_FIX_PARTIAL`.

## 2026-05-25 update — repair perimeter vs display perimeter

User feedback: fix produced a tiny z-fighting triangle but not the expected missing face.

Adjustment:

- build a dedicated repair candidate edge set per solid:
  - strict open-boundary edges,
  - plus linear context edges whose endpoints are both boundary vertices;
- run cycle extraction on that candidate graph (leaf-prune to 2-core, then closed loops on degree-2 edges);
- skip creating a cap if an equivalent loop face already exists (prevents duplicate-on-same-edges artifacts / z-fight);
- keep rendering highlight logic unchanged (visualization remains broader than strict repair constraints).

## 2026-05-25 update — planar hull fallback + fix counters

If cycle extraction yields no loops for a solid, try a conservative fallback:

- gather strict boundary vertices;
- require near-planarity;
- build a convex hull loop in the inferred boundary plane;
- cap only when all boundary points lie on the hull (no interior points).

Also surface Fix progress counters via partial message text (`loops detected`, `loops capped`, `skipped edges`) to speed debugging of unresolved cases.

## 2026-05-25 update — missing face with inner hole

User case: when the missing cap includes an inner hole, repair produced two independent faces (outer cap + filled hole) instead of one face with an inner loop.

Adjustment:

- group extracted loops by coplanar plane;
- project loops to 2D in that plane and sort by area;
- build face definitions as `outer + contained inner loops` and call `CreateFace` with multi-loop input;
- keep duplicate-face guard for multi-loop faces.
