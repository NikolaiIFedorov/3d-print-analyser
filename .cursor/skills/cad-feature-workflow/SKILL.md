---
name: cad-feature-workflow
description: >-
  Three-stage workflow for any CAD_OpenGL functionality change (feature, bug,
  behaviour): understand problem, implementation checklist, post-ship check.
  Use when starting or finishing a task, when the user asks for process, or
  when touching display/scene/input/logic modules.
---

# CAD_OpenGL — Feature development workflow

General process (theorize-first, TDD shape, critique) lives in the user's global `global_practices.md` (outside this repo, loaded automatically in Claude Code sessions; not visible to Cursor). This skill is the project-specific, Cursor-visible equivalent — see [practices/project_practices.md](../../../practices/project_practices.md) for this project's actual tools/conventions (Catch2, `docs/architecture.md`, `docs/todo/`, `session_log.json`). Project agents also get this via `.cursor/rules/cad-development-workflow.mdc` (`alwaysApply`).

There's no separate implementation-log journal in this project (an older `documentation/implementations/` convention is gone) — git commit messages are the record of what changed and why.

## Stage 1 — Understand the problem

Before code:

- Clarify expected vs actual behaviour (feature vs bug).
- Critique the idea: edge cases, at least one alternative.
- For bugfixes, gather runtime evidence first. Use **interactive debugging** (`lldb`/`gdb`), **live terminal logging** (`std::cout`), or **persistent logging** (`session_log.json`) to verify theories before changing implementation logic.
- Check `docs/todo/<target>.md` for the module, if it exists. Fix small isolated items in the same change; defer large redesigns.
- If the module is covered by `docs/architecture.md` (a tool's Algorithm, an Invariant, Validation's Types & fixes), read that section first — it's normative, not descriptive (see [practices/project_practices.md](../../../practices/project_practices.md) → `docs/architecture.md` is normative). A change that contradicts it needs an explicit decision, not a silent override.

Repeat until problem and approach are clear.

## Stage 2 — Implementation checklist

1. TDD — write tests first before implementation logic, in **Catch2** (separate test executable, run via `ctest`, never compiled into the app binary). Test real OCCT-touching logic against real OCCT calls; reserve hand-rolled fakes for orchestration/sequencing logic (Scene/Shared), not the geometry tools themselves. Tests assert what `docs/architecture.md` already documents — if a test would assert something the doc doesn't say, update the doc first.
2. Critique approach — edge cases, silent behaviour changes, alternative.
3. Architecture — SRP, DIP; right module (`display/`, `scene/`, `input/`, `logic/`); keep main-thread ownership for UI/input/navigation/render scheduling/OpenGL, and workers for heavy background work only (each Logic tool gets its own worker queue — see `docs/architecture.md` → Shared).
4. Performance — allocations, copies, per-frame heap; batch OpenGL.
5. Portability — `std::filesystem`, no platform assumptions.
6. Naming — camelCase / PascalCase; concise, unambiguous (see [practices/project_practices.md](../../../practices/project_practices.md) → Naming).
7. Consistency — match existing patterns.
8. DRY — duplicated logic; helpers / recursion vs fixed-depth copy-paste.
9. Post-implementation — re-read diff; dead code, magic numbers, unify divergent patterns.

SOLID vs performance: prefer performance when the cost is real; say why briefly.

## Stage 3 — After implementation

**If it works:** clean build → Stage 2 as review pass → commit with a descriptive message (problem, approach, main files — the actual record, since there's no separate log).

**If it fails:** test theories against `session_log.json` evidence; if stuck, return to Stage 1 and question whether the original approach was correct.
