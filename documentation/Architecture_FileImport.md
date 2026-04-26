# Architecture: File Import Pipeline

## Executive Summary

The file import pipeline brings 3D mesh data from disk into the scene graph. It has two tiers: a thin OS-level dialog wrapper (`FileImport`) and a set of format-specific parsers (`STLImport`, `OBJImport`, `ThreeMFImport`). `Display::DoFileImport()` is the integration point that wires them together, creates a new `Scene` per file, and triggers framing and UI updates.

**Verdict: Approve with changes**

**Top 3 strengths:**
1. Each format parser is a single static method with no shared mutable state — straightforward to add, test, or replace formats independently.
2. Binary/ASCII STL auto-detection based on file-size heuristic is correct and resilient to malformed ASCII headers.
3. `ThreeMFImport` correctly handles multi-`.model` archives and component reference chains.

**Top 3 concerns:**
1. STEP, PLY are advertised in the OS file filter but have no importer — selecting those formats silently creates an empty scene with no error feedback to the user.
2. An owned `Scene` is pushed onto `ownedScenes` and `activeSceneIndex` is updated *before* the format importer is called; if the importer returns `false`, the empty scene remains active with no rollback.
3. The SDL3 async callback allocates `FileCallback` on the heap via `new`; if the OS dialog is cancelled the `else` branch runs `delete callback` correctly, but the heap allocation pattern is non-RAII and deviates from the project's `unique_ptr` convention.

---

## 1. Requirements & Motivation

### Functional Requirements
- Present a native OS file picker filtered to supported 3D formats.
- Parse the selected file into the project's B-Rep `Scene` (Points, Edges, Faces, Solids).
- Support binary and ASCII STL, OBJ (triangles and N-gons), 3MF (zip + XML, component references).
- Assign a fresh `Scene` per imported file so multiple files can be open concurrently.
- Frame the viewport to the newly loaded model and trigger a scene analysis update.
- Record the import event in the session log and append a file tab to the UI.

### Non-Functional Requirements
- Parsing must complete synchronously within the file-selection callback thread (no async parse pipeline).
- Memory: each format parser uses stack-local containers; the final scene graph is heap-allocated and owned by `Display::ownedScenes`.

### Constraints
- Technology: C++23, SDL3 for the dialog API, miniz + tinyxml2 for 3MF, STL C++ streams for STL/OBJ.
- No STEP parser is in scope currently; the filter entry is aspirational.

---

## 2. Solution Description

### Components

| Component | Files | Responsibility |
|-----------|-------|----------------|
| `FileImport` | `src/input/FileImport.hpp/.cpp` | Wraps `SDL_ShowOpenFileDialog`; delivers selected path via a `std::function` callback |
| `STLImport` | `src/logic/Import/STLImport.hpp/.cpp` | Detects binary vs ASCII STL; parses triangles into Points, Edges, Faces; merges coplanar faces |
| `OBJImport` | `src/logic/Import/OBJImport.hpp/.cpp` | Parses `v` and `f` directives; handles N-gons and negative indices; creates a single Solid |
| `ThreeMFImport` | `src/logic/Import/ThreeMFImport.hpp/.cpp` | Extracts `.model` XML from ZIP; resolves component references recursively; creates a single Solid |
| `Display::DoFileImport()` | `src/display/display.cpp` | Orchestrates dialog → parser → scene lifecycle → UI update |

### Data Flow

```
User clicks "Import"
        │
        ▼
FileImport::OpenFileDialog(window, callback)
        │  (SDL3 async dialog; callback fires on file selection)
        ▼
callback(path)
  ├─ Create new Scene (push to ownedScenes, activate)
  ├─ Dispatch by extension:
  │     "stl"  → STLImport::Import(path, scene)
  │     "obj"  → OBJImport::Import(path, scene)
  │     "3mf"  → ThreeMFImport::Import(path, scene)
  │     other  → (no-op, silent empty scene)
  ├─ FrameScene()         — camera frames new model bounds
  ├─ UpdateScene()        — triggers analysis + renderer rebuild
  ├─ SessionLogger::LogFileImport()
  ├─ openFiles.push_back(filename)
  └─ RebuildFileTabs()    — refreshes file tab bar
```

### Key Abstractions

- **`FileImport::FileCallback`** — `std::function<void(const std::string&)>` alias; decouples the dialog from the import orchestration logic.
- **`STLImport` / `OBJImport` / `ThreeMFImport`** — stateless utility classes with a single `static bool Import(path, Scene*)` entry point; consistent interface enables easy addition of new formats.
- **`GetOrCreatePoint`** (STLImport, local) — point-deduplication helper using a `std::map<dvec3, Point*, Vec3Compare>`; ensures shared vertices produce shared `Point` objects in the B-Rep.

---

## 3. Design Principles

- **SRP**: Each importer handles exactly one format; `FileImport` handles only dialog presentation; `DoFileImport` handles only orchestration.
- **OCP**: Adding a new format requires one new class and one `else if` branch in `DoFileImport` — existing importers are not modified.
- **DIP** (partial): Importers receive a `Scene*` rather than constructing their own; they depend on the `Scene` abstraction, not on rendering or display concerns.
- **YAGNI**: No import abstraction base class exists — the current three formats share no common state, so a base class would add indirection without benefit.

---

## 4. Alternatives Considered

| Option | Pros | Cons | Verdict |
|--------|------|------|---------|
| **Current: static method per format** | Zero overhead, simple to read and test | `DoFileImport` extension point requires code edits | Chosen — format count is small |
| **Registry / map of factory functions** | `DoFileImport` becomes open to extension without modification | Adds indirection; formats are compile-time known | Deferred — worth reconsidering if format count grows |
| **Async parse on a worker thread** | Non-blocking UI during large file parse | Requires synchronization into scene graph; significant complexity | Deferred — files are typically small |
| **Single generic importer interface / base class** | Enables polymorphic dispatch | Formats have different I/O needs; interface would be artificial | Not recommended |

---

## 5. Technology & Dependencies

### Existing Dependencies Used

| Library | Version | License | Role |
|---------|---------|---------|------|
| SDL3 | (project-pinned) | zlib | Native OS file dialog (`SDL_ShowOpenFileDialog`) |
| miniz | bundled in `include/` | MIT | ZIP decompression for 3MF archives |
| tinyxml2 | bundled in `include/` | zlib | XML parsing of 3MF `.model` files |
| GLM | 1.0.2 | MIT | `dvec3` for vertex positions in STL point-deduplication |

All dependencies are already present in the project; no new build targets are required.

### Integration Impact
- `Display` includes `STLImport.hpp`, `OBJImport.hpp`, `ThreeMFImport.hpp`, `FileImport.hpp` — compile-time coupling to all importers.
- `ownedScenes` (vector of `unique_ptr<Scene>`) and `openFiles` (vector of filenames) live in `Display` — the scene lifetime is tied to the display object.

---

## 6. Tradeoffs

| Decision | Alternative Considered | Chosen Approach | Rationale |
|----------|----------------------|-----------------|-----------|
| One `Scene` per file | Single shared scene | Independent scenes per file | Enables multi-file tabs without scene merging complexity |
| Synchronous parse in SDL callback | Async worker thread | Synchronous | Simplicity; files are small enough in practice |
| `new FileCallback` for SDL async | `unique_ptr` + raw pointer cast | Raw `new`/`delete` | SDL3 `userdata` is `void*`; `unique_ptr::release/get` is the correct pattern but wasn't applied |
| Binary STL detection via file size | Header magic bytes | File-size formula (`84 + n*50`) | Robust — some ASCII STL files begin with "solid" which conflicts with binary headers |
| `std::map` for point deduplication | `std::unordered_map` | `std::map` with `Vec3Compare` | `glm::dvec3` has no `std::hash` specialisation; avoids a custom hash at the cost of O(log n) lookup |

---

## 7. Best Practices Compliance

### Conforming
- Each importer is stateless, side-effect-free (beyond mutating the `Scene*` argument), and short.
- Files are opened via `std::ifstream`; RAII ensures handles are closed on scope exit.
- `faces.reserve(triangleCount)` in binary STL avoids repeated reallocations.
- Degenerate triangles (duplicate vertices) are skipped before scene mutation.
- STL normal alignment is validated after face creation — outward normal is enforced for both binary (stored normal) and ASCII (CCW winding) variants.
- `ThreeMFImport` correctly releases the miniz archive handle (`mz_zip_reader_end`) before returning, even on the error path.

### Non-Conforming

| Issue | Severity | Remediation |
|-------|----------|-------------|
| **STEP and PLY listed in file filter but unimplemented** | Major | Either implement importers or remove those filter entries; current behaviour creates a silent empty scene |
| **Scene created before import success is known** | Major | Move `ownedScenes.push_back` / `activeSceneIndex` update to *after* a successful `Import()` call; roll back on failure |
| **`new FileCallback` / `delete callback`** | Minor | Use `unique_ptr<FileCallback>::release()` to produce the `void*` and re-wrap with `unique_ptr` inside the lambda for exception-safe cleanup |
| **OBJ: no `Solid` → `MergeCoplanarFaces` call** | Minor | OBJ creates a Solid but never calls `MergeCoplanarFaces`, unlike STL; coplanar face merging may be desirable for OBJ imports too |
| **`std::map` for point dedup in STLImport** | Minor | Consider a custom `std::unordered_map` with a spatial hash for O(1) average lookup on large meshes |

---

## 8. Risks & Future-Proofing

### Risks
- **Format coverage gap**: STEP is the dominant exchange format for CAD data; its absence means most mechanical CAD files cannot be imported without conversion. A STEP parser (e.g., via OpenCASCADE or a lightweight STEP reader) would significantly expand utility.
- **Large file performance**: The `std::map`-based point deduplication in STLImport is O(n log n). For meshes with millions of triangles, a spatial hash or sort-based deduplication would be meaningfully faster.
- **Single-mesh per file**: `ThreeMFImport` and `OBJImport` each create a single `Solid`; multi-object 3MF files with separate build items are flattened into one solid, losing object identity.

### Future Considerations
- A format-registry pattern (`std::unordered_map<string, ImportFn>`) would make `DoFileImport` open to extension without modification.
- Async import with a progress indicator becomes relevant once STEP or large mesh support is added.
- PLY support could be added with a relatively small parser; the filter entry already advertises it.
- Consider reporting import errors to the user via the UI (a status line in the Files panel) rather than only logging to the session log.

---

## 9. Recommendations

### Must-Have
- Remove STEP and PLY from the `SDL_DialogFileFilter` until their importers exist, or implement minimal parsers; the current state misleads users into selecting unsupported formats.
- Move scene creation to after a successful `Import()` return; roll back (pop `ownedScenes`, restore previous `scene` and `activeSceneIndex`) on failure or unsupported format.

### Should-Have
- Replace `new FileCallback` with `unique_ptr<FileCallback>::release()` + re-wrap inside the lambda.
- Call `scene->MergeCoplanarFaces(solid)` in `OBJImport::Import` (and `ThreeMFImport::Import`) for consistency with STLImport.
- Surface import errors to the user in the Files panel (e.g., a brief error line under the tab bar).

### Nice-to-Have
- Replace `std::map<dvec3, Point*, Vec3Compare>` with a spatial-hash deduplicator for large STL meshes.
- Add a basic PLY importer (binary and ASCII variants are straightforward).
- Investigate a format-registry architecture for `DoFileImport` to support Open/Closed extension.
