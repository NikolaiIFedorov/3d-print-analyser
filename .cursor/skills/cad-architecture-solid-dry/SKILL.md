---
name: cad-architecture-solid-dry
description: >-
  SOLID (with performance-over-purity when needed), DRY, pre/post implementation
  critique, and design discipline for CAD_OpenGL C++ code. Use when designing
  classes, interfaces, refactors, unifying code paths, or reviewing structure.
---

# CAD_OpenGL — Architecture, SOLID, DRY, critique

SOLID/DRY/critique/consistency below are general engineering practice (canonical version: the user's global `global_implementing_practices.md`, outside this repo, not visible to Cursor) — this skill's bullets are self-contained for Cursor sessions. The thread-ownership guardrail *is* project-specific — canonical detail there is [docs/architecture.md](../../../docs/architecture.md) → Architecture Layers → Shared, not restated in full below.

## Performance vs SOLID

When SOLID adds noticeable cost (indirection, virtuals in hot paths, extra allocations), **prioritize performance** and note the trade-off briefly.

## SOLID (short reminders)

- **SRP** — one reason to change; separate rendering, input, scene, business logic; keep shaders/geometry/display apart.
- **OCP** — extend without editing core; ABCs/interfaces for renderables, input, geometry; virtuals/templates over type-tag switches.
- **LSP** — subclasses honour base contracts; no narrowed preconditions or widened postconditions.
- **ISP** — small interfaces; split fat APIs (e.g. 2D vs 3D render).
- **DIP** — high-level code does not depend on GLFW/OpenGL; depend on abstractions; inject via constructors/factories.

## Pre-implementation critique

- **TDD:** Write unit tests for new logic before implementation.
- What edge cases are missed? What existing behaviour could break silently?
- **Evidence-First Debugging:** For bug work, use interactive debugging (`lldb`), live terminal logging, or `session_log.json` to validate the hypothesis before changing implementation logic.
- When replacing/unifying code: diff old paths, list every behavioural difference — each needs keep / drop / generalize.
- Consider at least one alternative; if no clear win, reconsider.

## Thread ownership guardrail

- Main thread owns UI/input/navigation/render scheduling, GLFW events, and OpenGL calls.
- Workers own heavy compute/IO and return results via explicit handoff (queue/message/future).
- Avoid direct worker mutation of scene/UI state; apply worker results on the main thread.

## DRY

- Same pattern in 2+ places → shared helper parameterized by differences.
- Hierarchies: prefer recursive traversal over fixed depth (avoids diverging per-level copies).
- When merging special cases: explicitly list behaviours the old paths had (missing one causes silent regressions).

## Post-implementation review

After it works: re-read diff for duplicates, magic numbers, naming drift, dead code. If the change introduces a pattern that exists elsewhere differently, unify one way.

## Consistency

Follow established patterns in related code; deviations need a concrete reason (e.g. performance), not convenience.
