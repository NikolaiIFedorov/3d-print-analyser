# Architecture: File Import Pipeline

**Cross-links:** [Architecture_UIThreadAndWorkers.md](Architecture_UIThreadAndWorkers.md) (main thread vs `TaskRunner`, cancel, progress), [Architecture_AsyncWorkRoadmap.md](Architecture_AsyncWorkRoadmap.md), [implementations/import_progress_phases_2026-05-04.md](implementations/import_progress_phases_2026-05-04.md).

## Executive Summary

The file import pipeline brings 3D mesh data from disk into the B-rep `Scene` graph. It combines:

1. **OS dialog** — `FileImport` / `SDL_ShowOpenFileDialog` delivers a path on a callback thread.
2. **Deferred handoff** — `Display::DoFileImport` only stores the path in `deferredImportPath` and marks the display dirty; **no parsing** runs in the dialog callback.
3. **Worker parse** — At the start of each `Display::Frame()`, `ProcessDeferredImportIfAny()` submits a `TaskRunner` job that runs `STLImport` / `OBJImport` / `ThreeMFImport` on a **background worker**, with optional progress reporting.
4. **Main-thread apply** — When the worker finishes, the main thread applies results through **`MainThreadPipeline`** named steps (`import-attach-scene`, `import-frame-scene`, `import-update-scene`, `import-finalize-ui`), each bounded by `MainThreadPipeline::Process` so large `UpdateScene` work can spread across frames.

**Strengths**

1. Format parsers remain **stateless static** entry points — easy to test and extend per format.
2. **STL** binary/ASCII detection and merge behaviour are mature; **3MF** handles multi-model archives and references.
3. **Parse work does not block** the SDL/ImGui/OpenGL frame loop; progress can surface during long imports.

**Gaps / risks**

1. **File filter vs importers:** The dialog still lists extensions (e.g. STEP, PLY) that the worker treats as **unsupported** (`result.ok == false`, progress message *"Unsupported import format."*). This avoids attaching an empty scene, but **UX is still weak** until filters match code or importers exist.
2. **SDL callback lifetime:** `FileImport` still uses heap-allocated `FileCallback` for SDL3 `userdata` (non-RAII pattern).
3. **Heavy main-thread steps:** `FrameScene`, `UpdateScene`, and GPU rebuild triggered from the apply pipeline can still be **large** for huge meshes — mitigated by `MainThreadPipeline` time budget, not eliminated.

---

## 1. Requirements & Motivation

### Functional

- Native file picker filtered to 3D formats the product may accept.
- Parse **STL** (binary + ASCII), **OBJ**, **3MF** into `Scene` (points, edges, faces, solids).
- One **owned `Scene` per imported file** (tab model); optional activation of the new tab when import was started as the “pending” tab.
- Frame camera to new bounds and refresh renderer / analysis when the imported tab is activated.
- Session logging (`LogFileImport`, STL merge diagnostics when present).
- **Progress** feedback during parse (phases / 0–1 progress where importers support callbacks).

### Non-functional

- **Responsiveness:** Parsing and disk I/O run on **`Display::taskRunner`** worker(s), not on the main UI thread.
- **Thread safety:** The worker mutates only a **`std::unique_ptr<Scene>`** inside `AsyncImportResult` until handoff; live `Display::scene` / `ownedScenes` are updated only from **`MainThreadPipeline`** steps on the main thread.
- **Stale UI:** `importProgressGeneration` gates published progress so superseded imports do not overwrite the UI.

### Constraints

- C++23, SDL3 dialog, miniz + tinyxml2 (3MF), streams for STL/OBJ.
- **No STEP / PLY parser** in-tree today — worker fails fast for unknown extensions.

---

## 2. Solution Description

### Components

| Piece | Location | Role |
|-------|----------|------|
| `FileImport` | `src/input/FileImport.cpp` | `SDL_ShowOpenFileDialog` + filters; invokes callback with path |
| `Display::DoFileImport` | `display.cpp` | Opens dialog; callback sets `deferredImportPath`, `renderDirty` |
| `Display::ProcessDeferredImportIfAny` | `display.cpp` | Starts `pendingImportTask` or polls `TryTake`; enqueues apply steps |
| `TaskRunner` / `TaskHandle` | `src/utils/TaskRunner.hpp` | Queue worker job; **non-blocking** `TryTake` on main thread |
| `MainThreadPipeline` | `src/utils/MainThreadPipeline.hpp` | Ordered main-thread steps with per-frame time budget |
| `STLImport` / `OBJImport` / `ThreeMFImport` | `src/logic/Import/` | Parse path → temporary `Scene` (worker thread only for this pipeline) |

### End-to-end data flow

```
User: Import
    │
    ▼
DoFileImport() → FileImport::OpenFileDialog(window, lambda)
    │
    │   (SDL async: callback may run on arbitrary thread)
    ▼
lambda(path): deferredImportPath = path; renderDirty = true
    │
    │   (next frames: main thread)
    ▼
Frame() → ProcessDeferredImportIfAny()
    │
    ├─ If no pending worker job:
    │      move deferredImportPath → path; importBusy = true; pendingImportTabStem; …
    │      mainThreadPipeline.Clear(); cancel prior import task if any
    │      pendingImportTask = taskRunner.Submit([path, importGeneration](token) {
    │          build AsyncImportResult:
    │            unique_ptr<Scene> on worker
    │            if stl/obj/3mf → Import(..., scene, progressCallback)
    │            else → progress "Unsupported…"; ok = false
    │            honour CancellationToken
    │            return result
    │      })
    │
    └─ If pendingImportTask && TryTake() ready:
           if !ok → clear busy / tab state; LOG_WARN; return
           enqueue MainThreadPipeline steps:
             import-attach-scene   → push_back(importedScene); maybe set activeSceneIndex
             import-frame-scene  → FrameScene() if activated tab
             import-update-scene → UpdateScene() if activated tab
             import-finalize-ui   → SessionLogger, RebuildFileTabs, Calibrate UI,
                                    Structure Begin if needed, FlushImportInputEventTail, clear busy
```

**Progress path:** Importers accept `ImportProgressCallback`; worker calls `PublishImportProgress(importGeneration, …)` → main thread `ApplyImportProgressSnapshot()` during `Frame()` so UI text stays coherent with the active import generation.

### Key abstractions

- **`AsyncImportResult`** (`display.hpp`) — path, extension lower-case, `ok` / `cancelled`, `unique_ptr<Scene>` populated only on success, optional STL timing / diagnostics for logging.
- **`ImportProgress` / `ImportProgressCallback`** — threaded parse reports phases without touching `Scene` on the main thread until apply.
- **Importers** — `static bool Import(path, Scene*, stats?, progress?)`; same contracts whether called from worker (live path) or tests.

---

## 3. Design Principles

- **SRP:** Importers know formats only; `Display` owns orchestration, tab state, and GL-facing apply.
- **OCP:** Adding a format extends the **worker branch** in `ProcessDeferredImportIfAny`’s submit lambda (and filters in `FileImport.cpp`), not the dialog API.
- **DIP:** Importers depend on `Scene*`, not on rendering.
- **Safety:** No `std::future::wait` on UI paths; `TaskHandle` avoids blocking `reset` when discarding incomplete work (see `TaskRunner` implementation notes in UI-thread architecture doc).

---

## 4. Alternatives Considered

| Option | Notes | Verdict |
|--------|------|---------|
| Parse in SDL callback | Blocks OS callback thread; freezes UI | Rejected for production path |
| **Worker + main apply (current)** | Progress possible; scene handoff explicit | **Chosen** |
| Registry of importers | Cleaner open/closed | Deferred — three `else if` branches today |

---

## 5. Technology & Dependencies

| Library | Role |
|---------|------|
| SDL3 | File dialog |
| miniz + tinyxml2 | 3MF ZIP + XML |
| GLM | Vertex / spatial types in STL dedup |

`Display` includes all importer headers — compile-time coupling unchanged.

---

## 6. Tradeoffs

| Topic | Choice | Rationale |
|-------|--------|-----------|
| Scene ownership | Worker builds `unique_ptr<Scene>`; main pushes to `ownedScenes` only after `ok` | Failed imports do **not** leave an empty active scene |
| Default `TaskRunner` worker count | 1 (see `display.hpp`) | Simple ordering; import rarely parallelized with other jobs on same runner |
| Apply step granularity | Four named `MainThreadPipeline` steps | Progress UX + avoids one giant synchronous block on success path |
| Filter breadth | Many extensions listed | Marketing / future formats vs confusing unsupported picks — **still a product tradeoff** |

---

## 7. Best Practices Compliance

### Conforming

- Importers remain stateless aside from mutating the passed `Scene*`.
- RAII file streams in parsers; 3MF releases miniz archive on all paths.
- STL coplanar merge and normal checks as before.

### Gaps (tracked)

| Issue | Severity | Remediation |
|-------|----------|-------------|
| STEP / PLY / etc. in filter without importer | Major (UX) | Narrow filters **or** add parsers **or** prominent in-app error when `ok == false` |
| `new FileCallback` in `FileImport` | Minor | `unique_ptr::release` + re-wrap pattern |
| OBJ / 3MF omit `MergeCoplanarFaces` where STL uses it | Minor | Align merge policy across formats if desired |
| Very large mesh: worker CPU + main `UpdateScene` | Medium | Profiling; mesh LOD; further chunking of apply steps |

---

## 8. Risks & Future Work

- **STEP** remains the largest format gap for mechanical CAD exchange.
- **Point dedup** in STL is `O(n log n)` with `std::map` — may matter for multi-million-triangle files on the worker (does not freeze UI, but extends wall time).
- **PLY** — filter advertises; worker returns unsupported until implemented.

---

## 9. Recommendations

### Must-have (product)

- Align **file filter entries** with **actually supported** extensions, **or** keep broad filter but add a clear **non-modal error** when import fails (unsupported / IO / parse).

### Should-have (engineering)

- Replace raw `new`/`delete` of `FileCallback` with a safer ownership handoff into the SDL callback.
- Consider `MergeCoplanarFaces` for OBJ/3MF for parity with STL.

### Nice-to-have

- Spatial-hash STL dedup on worker for huge meshes.
- Format registry to shrink the submit-lambda `if` chain when formats multiply.
