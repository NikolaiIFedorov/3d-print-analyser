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
| `Display::RefreshStructurePreviewForRenderer` | Per eligible face calls `StructureTriangulation::BuildFaceTriangulationPreview` (CGAL 2D offset / bake on **cache miss**) on **main thread** | **Follow-up:** debounce, cap faces/frame, or async preview job |

**Search used:** `rg` on `src/display`, `src/input`, `src/main.cpp` for `TryTake`, `TaskRunner`, `Submit`, `future`, `join`, `wait` (filtered for relevance).

## Outcome

- Added architecture contract: `documentation/Architecture_UIThreadAndWorkers.md`.
- Logged audit table and follow-ups in this file.
- `documentation/TODO`: listed new architecture doc under Written.
- `practices/best_practices.md`: Stage 3 architecture bullet points to the UI-thread/worker doc for async/CGAL display work.
- **Build:** `CAD_OpenGL` target succeeds (no C++ changes in this pass).

## Follow-ups (not done in this pass)

- Structure **preview** CGAL load: move or throttle (see audit table).
- Optional: explicit `CancelPending*` / reset handles in `Display::Shutdown()` before members destroy, if we ever need **deterministic** cancel ordering before GL teardown.
- Re-run grep when adding new async features or CGAL entry points.

## Mini retro

- **Worked:** Small policy doc + one audit pass gives a stable reference for reviews without refactoring CGAL preview immediately.
- **Did not:** No code change to preview path — largest remaining UI-thread CGAL cost is intentional deferral.
- **Skill / practices:** Consider a one-line pointer in `practices/best_practices.md` Development Workflow to this architecture file when touching `display.cpp` async sites.
