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
- Scan `documentation/TODO` for the module. Fix small isolated items in the same change; defer large redesigns.

Repeat until problem and approach are clear.

## Stage 2 — Documentation

- Check `documentation/implementations/` for an existing log on this problem or idea.
- **If none:** create a log (append only). Capture: idea, implementation plan, bugs encountered, patch attempts, outcome.

## Stage 3 — Implementation checklist

1. Critique approach — edge cases, silent behaviour changes, alternative.
2. Architecture — SRP, DIP; right module (`display/`, `scene/`, `input/`, `logic/`).
3. Performance — allocations, copies, per-frame heap; batch OpenGL.
4. Portability — `std::filesystem`, no platform assumptions.
5. Naming — camelCase / PascalCase; concise, unambiguous.
6. Consistency — match existing patterns.
7. DRY — duplicated logic; helpers / recursion vs fixed-depth copy-paste.
8. Post-implementation — re-read diff; dead code, magic numbers, unify divergent patterns.

SOLID vs performance: prefer performance when the cost is real; say why briefly.

## Stage 4 — After implementation

**If it works:** clean build → Stage 3 as review → **mini retro** (what worked, what failed, concrete edits for skills or `best_practices.md`) → commit → append implementation log (outcome, optional retro).

**If it fails:** log failure and theories in the implementation log; test theories; if stuck, return to Stage 1.

Optional: append a short retro paragraph to the relevant file under `documentation/implementations/`.
