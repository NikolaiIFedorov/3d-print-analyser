---
name: cad-architecture-solid-dry
description: >-
  SOLID (with performance-over-purity when needed), DRY, pre/post implementation
  critique, and design discipline for CAD_OpenGL C++ code. Use when designing
  classes, interfaces, refactors, unifying code paths, or reviewing structure.
---

# CAD_OpenGL — Architecture, SOLID, DRY, critique

Canonical detail: [practices/best_practices.md](../../../practices/best_practices.md) (SOLID, DRY, Pre-Implementation Critique, Post-Implementation Review, Consistency).

## Performance vs SOLID

When SOLID adds noticeable cost (indirection, virtuals in hot paths, extra allocations), **prioritize performance** and note the trade-off briefly.

## SOLID (short reminders)

- **SRP** — one reason to change; separate rendering, input, scene, business logic; keep shaders/geometry/display apart.
- **OCP** — extend without editing core; ABCs/interfaces for renderables, input, geometry; virtuals/templates over type-tag switches.
- **LSP** — subclasses honour base contracts; no narrowed preconditions or widened postconditions.
- **ISP** — small interfaces; split fat APIs (e.g. 2D vs 3D render).
- **DIP** — high-level code does not depend on GLFW/OpenGL; depend on abstractions; inject via constructors/factories.

## Pre-implementation critique

- What edge cases are missed? What existing behaviour could break silently?
- When replacing/unifying code: diff old paths, list every behavioural difference — each needs keep / drop / generalize.
- Consider at least one alternative; if no clear win, reconsider.

## DRY

- Same pattern in 2+ places → shared helper parameterized by differences.
- Hierarchies: prefer recursive traversal over fixed depth (avoids diverging per-level copies).
- When merging special cases: explicitly list behaviours the old paths had (missing one causes silent regressions).

## Post-implementation review

After it works: re-read diff for duplicates, magic numbers, naming drift, dead code. If the change introduces a pattern that exists elsewhere differently, unify one way.

## Consistency

Follow established patterns in related code; deviations need a concrete reason (e.g. performance), not convenience.
