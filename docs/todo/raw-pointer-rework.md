# Raw-pointer → TopoDS_Face value-semantics rework

Tracks every known structure still keyed off raw pointers from the pre-OCCT-rewrite model, predating the value-semantics invariant in `architecture.md` (sub-shapes referenced by `TopoDS_Face` value, never raw pointer).

- **Analysis** — `FaceFlaw`/`AnalysisResults` key off raw `const Face *`/`const Solid *`, with a `PruneDefunctAnalysisResults` workaround for staleness. Needs `TopoDS_Face`-by-value rework; should let that workaround be deleted entirely. `AnalysisResults.bridgeSurfaces` needs the same treatment. See [analysis-redesign.md](analysis-redesign.md).
- **Calibrate** — `CalibDistanceType.hpp` takes raw `const Face *`/`const Edge *`/`const Scene *`. Same rework as Analysis's `FaceFlaw`.
