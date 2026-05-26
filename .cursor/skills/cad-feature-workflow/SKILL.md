---
name: cad-feature-workflow
description: >-
  Four-stage workflow for any CAD_OpenGL functionality change (feature, bug,
  behaviour): understand problem, documentation/implementations log, implementation
  checklist, post-ship mini retro. Use when starting or finishing a task, when the
  user asks for process, or when touching display/scene/input/logic modules.
---

# CAD_OpenGL — Feature development workflow

Canonical detail: [practices/best_practices.md](../../../practices/best_practices.md) (Development Workflow, Mini retrospective). Project agents also get this via `.cursor/rules/cad-development-workflow.mdc` (`alwaysApply`).

## Stage 1 — Understand the problem

Before code:

- Clarify expected vs actual behaviour (feature vs bug).
- Critique the idea: edge cases, at least one alternative.
- For bugfixes, confirm the failure pattern in `session_log.json` first. Use **log-first debugging**: verify theories by adding logging before changing implementation logic.
- Scan `documentation/TODO` for the module. Fix small isolated items in the same change; defer large redesigns.

Repeat until problem and approach are clear.

## Stage 2 — Documentation

- Check `documentation/implementations/` for an existing log on this problem or idea.
- **If none:** create a log (append only). Capture: idea, implementation plan, bugs encountered, patch attempts, outcome.

## Stage 3 — Implementation checklist

1. TDD — write tests first before implementation logic.
2. Critique approach — edge cases, silent behaviour changes, alternative.
3. Architecture — SRP, DIP; right module (`display/`, `scene/`, `input/`, `logic/`); keep main-thread ownership for UI/input/navigation/render scheduling/OpenGL, and workers for heavy background work only.
4. Performance — allocations, copies, per-frame heap; batch OpenGL.
5. Portability — `std::filesystem`, no platform assumptions.
6. Naming — camelCase / PascalCase; concise, unambiguous.
7. Consistency — match existing patterns.
8. DRY — duplicated logic; helpers / recursion vs fixed-depth copy-paste.
9. Post-implementation — re-read diff; dead code, magic numbers, unify divergent patterns.

SOLID vs performance: prefer performance when the cost is real; say why briefly.

## Stage 4 — After implementation

**If it works:** clean build → Stage 3 as review → if debugging effort was meaningful (multiple failed theories/attempts, not fixed in one clean try), run **mini retro** (what worked, what failed, what triggered the bug/root cause, concrete edits for skills or `best_practices.md`) → commit → append implementation log (outcome, optional retro).

**If it fails:** log failure and theories in the implementation log; test theories against `session_log.json` evidence; if stuck, return to Stage 1.

Optional: append a short retro paragraph to the relevant file under `documentation/implementations/`.
