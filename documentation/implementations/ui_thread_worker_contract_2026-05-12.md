# UI thread vs workers — contract rollout and audit

## Problem / idea

Structure / CGAL work exposed **main-thread freezes** and fragile **async handle** teardown. We want a **documented contract** plus a **grep audit** of hot paths so new tools do not regress.

## Plan

1. Add `documentation/Architecture_UIThreadAndWorkers.md` (policy + CGAL + shutdown notes).
2. Audit `src/display`, `src/input`, `src/main.cpp` for blocking patterns and CGAL call sites.
3. Update `documentation/TODO` architecture list.
4. Clean build (sanity) and commit.

## Audit snapshot (2026-05-12)

| Area | Finding | Target |
|------|---------|--------|
| `TaskRunner::TaskHandle` | `TryTake` uses `future.get()` only when `wait_for(0)` reports ready; destructor / move-assign use `ReleaseFutureNonBlocking` | Keep as single pattern for all `Submit` handles |
| `TaskRunner::~TaskRunner` | `join()` on worker threads | Acceptable for clean shutdown; can still **block** if worker is inside one long CGAL call — document, optional future: stop + timeout / detach policy |
| `src/input` | No `TryTake` / `future` / `join` hits | OK |
| `src/main.cpp` | No direct future/join hits in quick scan | OK |
| `Display` async | Import, analysis, structure staging use `Submit` + `TryTake` in `Frame()` / poll helpers | OK — pattern to copy |
| `Display::RefreshStructurePreviewForRenderer` | Was: all eligible faces baked in one call. | **Done (2026-05-12):** incremental bake via `TickStructurePreviewBuildIfNeeded` (max 8 faces/frame); optional later: async preview job. |

**Search used:** `rg` on `src/display`, `src/input`, `src/main.cpp` for `TryTake`, `TaskRunner`, `Submit`, `future`, `join`, `wait` (filtered for relevance).

## Outcome

- Added architecture contract: `documentation/Architecture_UIThreadAndWorkers.md`.
- Logged audit table and follow-ups in this file.
- `documentation/TODO`: listed new architecture doc under Written.
- `practices/best_practices.md`: Stage 3 architecture bullet points to the UI-thread/worker doc for async/CGAL display work.
- **Build:** `CAD_OpenGL` target succeeds (no C++ changes in this pass).

## Follow-up implementation (2026-05-12) — Structure preview chunking

- **Problem:** `RefreshStructurePreviewForRenderer` called `BuildFaceTriangulationPreview` for every eligible face in one go — CGAL could stall the UI thread on large eligibility sets.
- **Approach:** Stable sorted work order + snapshot match on `(insetMm, face set)`; accumulate segments in `structurePreviewBakedSegments`; advance `structurePreviewBakeCursor` by at most `kStructurePreviewMaxFacesPerFrame` (8) per `TickStructurePreviewBuildIfNeeded`. `RefreshStructurePreviewForRenderer` only updates queue / clears baked segments when the snapshot changes; `TickStructurePreviewBuildIfNeeded` runs at end of the same `Frame()` pass as `RunPickNode()` so pick/eligibility updates apply before continuing a bake.
- **Files:** `src/display/display.hpp`, `src/display/display.cpp`; `Display::Shutdown` resets incremental preview and cancels pending Structure carve when `CAD_USE_CGAL`.

## Outcome (chunking pass)

- Incremental Structure preview baking shipped; architecture doc CGAL bullet updated.
- `Display::Shutdown` clears incremental preview state and calls `CancelPendingStructureCarveJob` when CGAL is enabled.
- **Build:** `CAD_OpenGL` OK.

## Follow-ups (remaining)

- Optional: async Structure preview job for zero main-thread CGAL; time budget per frame.
- `Display::Shutdown` now resets incremental preview state and cancels pending **Structure** carve (`CAD_USE_CGAL`); import/analysis handles still rely on `Display` member destruction order.
- Re-run grep when adding new async features or CGAL entry points.

## Mini retro

- **Worked:** Contract doc + audit table gave a clear place to record the chunking follow-up; per-frame cap integrates without new threads.
- **Did not:** Single-face `BuildFaceTriangulationPreview` can still be slow on pathological geometry; async preview job still optional.
- **Skill / practices:** `best_practices.md` already links the architecture doc from Stage 3.
