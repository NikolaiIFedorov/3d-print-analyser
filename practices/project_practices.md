# Project Practices

What's specific to CAD_OpenGl. General engineering principles (SOLID, DRY, critique, TDD process shape, first-principles theorizing) live in the global `global_practices.md`, loaded automatically via `~/.claude/CLAUDE.md` — this file holds only what that global default can't know: this project's chosen tools, conventions, and file locations. If something here just restates a generic practice, it's been cut on purpose, not omitted by accident.

---

## `docs/architecture.md` is normative

The general principle ("written design docs are normative") lives in `global_practices.md` #process. The CAD_OpenGl-specific instantiation: the doc is `docs/architecture.md`, and it covers Invariants, each tool's Algorithm, and Validation's Types & fixes. A change that contradicts one of those needs an explicit decision — fix the code, or revise the doc with reasoning — not a silent override.

---

## Policy-Skill Sync

- When `practices/project_practices.md` changes, run the `sync-skills-from-best-practices` skill to align `.cursor/skills/*` with the updated policy.

---

## Testing — Catch2

- **Catch2**, as a separate test executable/target — never compiled into the shipping app binary. Run via CTest (`ctest`), a distinct step after `cmake --build .`, not triggered automatically by the build itself.
- Test the actual OCCT-touching logic (Structure's carve, Validation's heal/recheck, Analysis's detectors) against **real OCCT calls** — it's local and deterministic, so there's no flakiness/cost to avoid, and a mock would only assert what you already assumed instead of verifying the geometry kernel actually did the right thing.
- Reserve hand-rolled fakes for **orchestration/sequencing logic** — e.g. does Scene commit only after a worker job succeeds, does Analysis get submitted only after a commit. Catch2 has no built-in mocking; reach for GoogleMock alongside it only if hand-rolled fakes stop being enough.
- Tests assert what `docs/architecture.md` already documents. If a test would assert something the doc doesn't say, the doc is incomplete — update it before or alongside the test.
- Some invariants belong in the type system instead of a test — stronger, since violating them won't compile rather than just being caught at test time (e.g. `Part`'s mutators private with `Scene` as the only friend, an Operation's signature shape enforcing "always returns a new shape"). Prefer that where possible before reaching for a runtime test.

---

## Debugging

- For complex state or crashes, prefer an interactive debugger (`lldb`/`gdb`) over print statements.
- For fast iteration, temporary `std::cout`/`printf` in the live terminal.
- For tracing historical events across frames, add targeted logging to `session_log.json` first and verify the hypothesis against it before changing implementation logic — don't patch logic by trial and error.

---

## Build & dependencies

- CMake is the single source of truth for builds — no IDE-specific project files.
- Windowing through GLFW, OpenGL functions loaded through GLAD — never platform-native window APIs directly.
- Vertex Array/Buffer Objects for geometry upload, batched draw calls, no immediate-mode rendering.
- Third-party libraries live under `include/<name>-<version>/`, version number in the directory name.
- C++23, no CGAL (removed — OCCT covers everything CGAL used to).

---

## Threading

Main thread owns UI/input/render scheduling/OpenGL calls; each Logic tool gets its own worker queue, not a shared pool — see `docs/architecture.md` → Architecture Layers → Shared for the full reasoning and the per-tool-queue algorithm. Don't restate that here; it'll drift from the canonical version.

---

## Naming

- Concise names, qualified just enough to disambiguate within the surrounding context; qualifier goes at the front (`patchIndices`, not `indicesPatch`).
- Avoid redundant context — inside a `Wireframe` class, `indices`, not `wireframeIndices`.
- camelCase for variables/functions, PascalCase for types/classes.

---

## Where things live

- `docs/architecture.md` — normative domain decisions and algorithms (see above).
- `docs/todo/<target>.md` — per-target known issues; check before touching a module, written/refreshed by `/audit`.
- `session_log.json` — runtime event/state log for debugging (see Debugging above).
- There is currently no separate implementation-log journal (an older `documentation/implementations/` convention referenced by past commit messages no longer exists in this tree) — git commit messages are the record of what changed and why.
